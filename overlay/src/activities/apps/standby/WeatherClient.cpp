#include "WeatherClient.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include "CalendarConfig.h"
#include "CalendarLog.h"
#include "CalmodWifi.h"
#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "activities/RenderLock.h"
#include "network/HttpDownloader.h"

#if CROSSPOINT_EMULATED == 0
#include <esp_heap_caps.h>
#endif

namespace {

constexpr const char* kCachePath = "/.crosspoint/calendar_cache.json";
constexpr const char* kHolidaysPath = "/.crosspoint/calendar_holidays.json";
constexpr const char* kSettingsPath = "/.crosspoint/calendar_settings.json";

// Тайминги — в CalendarConfig.h.
constexpr uint32_t kFirstDelayMs = calendar_config::kFirstRequestDelaySec * 1000u;
constexpr uint32_t kConnectTimeoutMs = calendar_config::kWifiConnectTimeoutSec * 1000u;
constexpr uint32_t kBusyRetryMs = calendar_config::kRetryWifiBusySec * 1000u;
constexpr uint32_t kBusyTakeoverMs = calendar_config::kWifiBusyTakeoverSec * 1000u;
constexpr uint32_t kFailRetryMs = calendar_config::kRetryAfterFailMin * 60u * 1000u;
constexpr uint32_t kNoCredsRetryMs = calendar_config::kRetryNoWifiMin * 60u * 1000u;
constexpr uint32_t kLowBatteryRetryMs = calendar_config::kRetryLowBatteryMin * 60u * 1000u;
constexpr size_t kMaxBody = 4096;         // погода, место, календарь: сотни байт … 2 КБ; больше — явно не то (не настройка)
constexpr size_t kMaxSearchBody = 16384;  // поиск города: у мест бывают длинные списки почтовых индексов
// TLS-рукопожатие wolfSSL + разбор JSON. Upstream даёт такой работе 16 КБ («8 КБ переполняется на TLS + JSON»,
// platformio.ini, loop task); прежних 10 КБ было впритык. Стек занят только пока идёт выход в сеть. (не настройка)
constexpr uint32_t kTaskStackBytes = 16384;
constexpr uint32_t kDueCheckEveryMs = 1000;
constexpr uint32_t kScanPerChannelMs = 300;   // активный скан: ≈ 13 каналов × 0,1–0,3 с
constexpr uint32_t kStatusGraceMs = 3000;     // раньше этого отказу по статусу не верим (см. connectWifi)

// Одна сетевая задача за раз на всё устройство: задача, брошенная закрытой гранью, ещё может работать.
std::atomic<bool> g_netBusy{false};

int yearOfEpoch(uint32_t epoch) {
  int y;
  unsigned m, d;
  calendar_core::civilFromDays(static_cast<int32_t>(epoch / 86400u), y, m, d);
  return y;
}

bool hasAddress() { return WiFi.localIP()[0] != 0; }  // 0.x.x.x DHCP не выдаёт: «адреса ещё нет»

void heapInfo(char* out, size_t n) {
#if CROSSPOINT_EMULATED == 0
  std::snprintf(out, n, "память: своб. %u (кусок %u), PSRAM %u", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
#else
  std::snprintf(out, n, "память: своб. %u", static_cast<unsigned>(ESP.getFreeHeap()));
#endif
}

const char* wlName(int st) {
  switch (st) {
    case WL_IDLE_STATUS:
      return "idle";
    case WL_NO_SSID_AVAIL:
      return "сеть не найдена";
    case WL_CONNECTED:
      return "подключено";
    case WL_CONNECT_FAILED:
      return "отказ (пароль?)";
    case WL_DISCONNECTED:
      return "отключено";
    default:
      return "?";
  }
}

// Причины обрыва Wi-Fi (коды ESP-IDF) — в журнал: подписываемся на события Wi-Fi один раз за загрузку.
void watchWifiEvents() {
#if CROSSPOINT_EMULATED == 0
  static bool subscribed = false;
  if (subscribed) return;
  subscribed = true;
  Network.onEvent([](arduino_event_id_t e, arduino_event_info_t info) {
    switch (e) {
      case ARDUINO_EVENT_WIFI_STA_CONNECTED: {
        const auto& c = info.wifi_sta_connected;
        cal_log::line("WIFI", "событие: связь с точкой «%.*s», канал %u, защита %d", static_cast<int>(c.ssid_len),
                      reinterpret_cast<const char*>(c.ssid), static_cast<unsigned>(c.channel), static_cast<int>(c.authmode));
        break;
      }
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        const auto& d = info.wifi_sta_disconnected;
        cal_log::line("WIFI", "событие: отключение от «%.*s», причина %u (%s), сигнал %d дБм", static_cast<int>(d.ssid_len),
                      reinterpret_cast<const char*>(d.ssid), static_cast<unsigned>(d.reason),
                      WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(d.reason)), static_cast<int>(d.rssi));
        break;
      }
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        cal_log::line("WIFI", "событие: получен адрес %s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
        break;
      case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        cal_log::line("WIFI", "событие: адрес потерян");
        break;
      default:
        break;
    }
  });
#endif
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Задача: вход → результат. Принадлежит либо задаче (пока работает), либо грани (после «готово»), либо снова задаче
// (если грань закрылась раньше): кто последним «отпустил» — тот и удаляет. Состояние решает атомарный CAS.
// ---------------------------------------------------------------------------------------------------------------------
struct WeatherClient::Job {
  static constexpr unsigned kMaxYearsPerJob = calendar_config::kHolidayMaxRequestsPerCycle;
  struct Candidate {
    std::string ssid, pass;
  };
  static constexpr unsigned kMaxCandidates = 8;  // столько же, сколько WifiCredentialStore::MAX_NETWORKS (private)

  // Вход.
  GfxRenderer* renderer = nullptr;
  uint32_t nowEpoch = 0;
  calendar_core::Lang lang = calendar_core::Lang::En;
  bool alreadyConnected = false;  // Wi-Fi поднят не нами — не выключаем
  // Все сохранённые сети (startJob() читает их под RenderLock), не только последняя: подключаемся к той, что сейчас рядом.
  Candidate candidates[kMaxCandidates];
  unsigned nCandidates = 0;
  std::string lastSsid;  // WIFI_STORE.getLastConnectedSsid(): не видна в скане — пробуем её вслепую (скрытая сеть)
  bool autoLocation = true;
  bool forceGeo = false;
  weather_core::Place ipPlace;  // последнее место по IP (и через какую сеть определено)
  weather_core::Place target;   // для какого места качать погоду; в «Авто» может смениться после геолокации
  bool doWeather = false;
  unsigned nYears = 0;
  int years[kMaxYearsPerJob] = {};
  char searchQuery[64] = "";    // непусто — найти город по названию

  // Результат.
  bool netOk = true;        // сеть в целом отвечала
  bool geoOk = false;
  weather_core::Place newPlace;
  bool wxTried = false;
  bool wxOk = false;
  weather_core::Weather nw;
  bool fcOk = false;
  weather_core::Forecast nf;
  struct HolYear {
    int year;
    holiday_core::YearData data;
  };
  HolYear hol[kMaxYearsPerJob];
  unsigned nHol = 0;
  bool holBad = false;      // сервис вернул не то — не долбим сразу снова
  bool holFailed = false;   // хотя бы один год не скачался — повторим позже
  int searchResult = -2;    // -2 — не искали (нет сети); -1 — не удалось; 0.. — сколько нашли
  weather_core::GeoHit hits[weather_core::kMaxGeoHits];
  std::string ourSsid;      // к какой сети подключились сами: applyJob() запомнит её как последнюю

  // 0 — работает; 1 — готово, владелец грань; 2 — брошено гранью, владелец задача.
  std::atomic<int> state{0};

  void run();

 private:
  bool ownsWifi = false;
  bool abandoned() const { return state.load(std::memory_order_acquire) == 2; }
  bool connectWifi();
  void releaseWifi();
  bool fetch(const char* url, std::string& out, size_t maxBytes, const char* what);
};

bool WeatherClient::Job::fetch(const char* url, std::string& out, size_t maxBytes, const char* what) {
  const uint32_t t = millis();
  const bool ok = HttpDownloader::fetchUrl(url, out);
  const bool fits = !out.empty() && out.size() <= maxBytes;
  cal_log::line("HTTP", "%s: %s, %u Б, %lu мс", what, !ok ? "ОШИБКА (подробности — строкой [HTTP] прошивки выше)" : fits ? "ok" : "пустой или слишком большой ответ",
                static_cast<unsigned>(out.size()), static_cast<unsigned long>(millis() - t));
  return ok && fits;
}

// Подключение к самой сильной из запомненных сетей, что сейчас видны; не вышло — следующая (до kWifiMaxAttempts).
bool WeatherClient::Job::connectWifi() {
  WiFi.persistent(false);
  if (!NetworkStartup::setMode(*renderer, WIFI_STA)) {
    cal_log::line("WIFI", "не удалось включить Wi-Fi (WiFi.mode)");
    return false;
  }
  ownsWifi = true;  // радио теперь наше — выключить в конце, даже если подключаться окажется не к чему
  watchWifiEvents();
#if CROSSPOINT_EMULATED == 0
  // Станция стартует асинхронно (событие STA_START): до него стереть прежнюю конфигурацию нельзя.
  for (int i = 0; i < 20 && !WiFi.STA.started(); ++i) delay(50);
#endif
  // «Чистый лист», НЕ выключая радио: WiFi.disconnect(true, …) в этой версии Arduino-ESP32 полностью останавливает и
  // освобождает Wi-Fi, и скан сразу после этого стартовал на ещё не поднявшемся стеке (и мог не удаться — тогда
  // оставалась только прошлая сеть, то есть дома — рабочая).
  WiFi.disconnect(false, true);
#if CROSSPOINT_EMULATED == 0
  // Прерванный кем-то скан оставляет флаг «идёт скан», и все следующие сразу отвечают «занято». scanComplete() снимает
  // его, если скан «идёт» дольше таймаута; штатные 60 с для этого слишком долго.
  WiFi.setScanTimeout(10000);
  (void)WiFi.scanComplete();
#endif

  const uint32_t ts = millis();
  int16_t found = WiFi.scanNetworks(false, false, false, kScanPerChannelMs);
  if (found < 0) {
    cal_log::line("WIFI", "скан не удался (%d) — повтор", static_cast<int>(found));
    delay(500);
    found = WiFi.scanNetworks(false, false, false, kScanPerChannelMs);
  }
  // У каждой запомненной сети — лучший сигнал среди её точек доступа.
  int32_t best[kMaxCandidates] = {};
  bool seen[kMaxCandidates] = {};
  std::string others;
  int nOthers = 0;
  for (int16_t i = 0; i < found; ++i) {
    const String s = WiFi.SSID(i);
    const int32_t rssi = WiFi.RSSI(i);
    bool saved = false;
    for (unsigned c = 0; c < nCandidates; ++c) {
      if (s != candidates[c].ssid.c_str()) continue;
      saved = true;
      if (!seen[c] || rssi > best[c]) best[c] = rssi;
      seen[c] = true;
    }
    if (!saved && !s.isEmpty() && nOthers < 10) {
      char one[48];
      std::snprintf(one, sizeof(one), "%s«%s» %ld", nOthers ? ", " : "", s.c_str(), static_cast<long>(rssi));
      others += one;
      ++nOthers;
    }
  }
  WiFi.scanDelete();

  // Порядок попыток: видимые запомненные — от сильного сигнала к слабому.
  unsigned order[kMaxCandidates];
  unsigned nOrder = 0;
  for (unsigned c = 0; c < nCandidates; ++c) {
    if (seen[c]) order[nOrder++] = c;
  }
  std::sort(order, order + nOrder, [&](unsigned a, unsigned b) { return best[a] > best[b]; });
  std::string savedList;
  for (unsigned k = 0; k < nOrder; ++k) {
    char one[48];
    std::snprintf(one, sizeof(one), "%s«%s» %ld", k ? ", " : "", candidates[order[k]].ssid.c_str(),
                  static_cast<long>(best[order[k]]));
    savedList += one;
  }
  cal_log::line("WIFI", "скан: %d сетей за %lu мс; запомненные рядом: %s; прочие: %s", static_cast<int>(found),
                static_cast<unsigned long>(millis() - ts), nOrder ? savedList.c_str() : "нет", nOthers ? others.c_str() : "—");
  if (nOrder == 0) {
    for (unsigned c = 0; c < nCandidates; ++c) {
      if (!lastSsid.empty() && candidates[c].ssid == lastSsid) order[nOrder++] = c;
    }
    if (nOrder) cal_log::line("WIFI", "запомненных сетей не видно — пробую последнюю «%s» вслепую (вдруг скрытая)", lastSsid.c_str());
  }
  if (nOrder == 0) {
    cal_log::line("WIFI", "подключаться не к чему: запомненных сетей рядом нет");
    return false;
  }

  const unsigned attempts = std::min<unsigned>(nOrder, calendar_config::kWifiMaxAttempts);
  for (unsigned a = 0; a < attempts && !abandoned(); ++a) {
    const Candidate& cd = candidates[order[a]];
    const bool enterprise = !cd.pass.empty() && cd.pass[0] == calmod_wifi::kSep;
    const uint32_t a0 = millis();
    calmod_wifi::begin(cd.ssid.c_str(), cd.pass.c_str());  // обычный пароль, открытая сеть или логин+пароль (PEAP)
    const uint32_t limit = calmod_wifi::timeoutMs(kConnectTimeoutMs);  // Enterprise — дольше
    wl_status_t st = WL_IDLE_STATUS;
    bool ok = false;
    for (;;) {
      delay(100);
      st = WiFi.status();
      const uint32_t el = millis() - a0;
      if (st == WL_CONNECTED && hasAddress()) {
        ok = true;
        break;
      }
      if (el >= limit || abandoned()) break;
      // Статус Arduino меняют события, и сразу после begin() он может быть ещё от прошлой попытки: отказу верим не
      // раньше kStatusGraceMs, до того просто ждём.
      if (el >= kStatusGraceMs && (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL)) break;
    }
    const unsigned long el = millis() - a0;
    if (ok) {
      cal_log::line("WIFI", "подключено к «%s»%s за %lu мс: адрес %s, сигнал %d дБм, канал %d", cd.ssid.c_str(),
                    enterprise ? " (Enterprise)" : "", el, WiFi.localIP().toString().c_str(), static_cast<int>(WiFi.RSSI()),
                    static_cast<int>(WiFi.channel()));
      ourSsid = cd.ssid;
      return true;
    }
    cal_log::line("WIFI", "«%s»%s: не подключилось за %lu мс из %lu (статус %d: %s)", cd.ssid.c_str(),
                  enterprise ? " (Enterprise)" : "", el, static_cast<unsigned long>(limit), static_cast<int>(st), wlName(st));
    WiFi.disconnect(false, true);
    delay(200);
  }
  return false;
}

void WeatherClient::Job::releaseWifi() {
  if (!ownsWifi) return;
  ownsWifi = false;
  WiFi.disconnect(false);  // событие «отключено» успевает обработаться до выключения — статус не остаётся «подключено»
  delay(100);
  WiFi.mode(WIFI_OFF);
  // Как StandbyActivity::stopTimeSyncWifi: без этого CPU не опускается на LOW_POWER_FREQ. В этой версии Arduino-ESP32
  // WiFi.mode(WIFI_OFF) уже освобождает драйвер — тогда это безвредный повтор.
  esp_wifi_deinit();
}

void WeatherClient::Job::run() {
  const uint32_t t0 = millis();
  const uint32_t budgetMs = calendar_config::kNetworkBudgetSec * 1000u;
  char mem[96];
  heapInfo(mem, sizeof(mem));
  cal_log::line("NET", "выход в сеть: погода %s, лет календаря %u, поиск «%s»; место %s; %s", doWeather ? "да" : "нет",
                nYears, searchQuery, autoLocation ? "авто" : "вручную", mem);

  char ssidNow[33] = "";
  if (alreadyConnected) {
    weather_core::copyUtf8(ssidNow, sizeof(ssidNow), WiFi.SSID().c_str());
    cal_log::line("WIFI", "уже подключено (не нами) к «%s»: адрес %s, сигнал %d дБм", ssidNow,
                  WiFi.localIP().toString().c_str(), static_cast<int>(WiFi.RSSI()));
  } else if (connectWifi()) {
    weather_core::copyUtf8(ssidNow, sizeof(ssidNow), ourSsid.c_str());
  } else {
    netOk = false;
  }

  if (netOk && !abandoned()) {
    std::string body;
    char url[700];  // URL прогноза ≈ 450 символов
    int failStreak = 0;
    // Бюджет — на запросы, от момента подключения: само подключение ограничено отдельно (Enterprise — до 45 с на попытку).
    const uint32_t tNet = millis();
    // Запрос идёт, только пока есть бюджет времени, грань не закрылась и сеть не «легла» (два подряд провала).
    auto get = [&](const char* u, size_t maxBytes, const char* what) {
      if (abandoned()) return false;
      if (millis() - tNet >= budgetMs || failStreak >= 2) {
        cal_log::line("NET", "%s: пропущено (%s)", what, failStreak >= 2 ? "два запроса подряд не удались" : "исчерпан бюджет времени");
        return false;
      }
      const bool ok = fetch(u, body, maxBytes, what);
      failStreak = ok ? 0 : failStreak + 1;
      return ok;
    };

    // Поиск города — первым: пользователь ждёт ответа на экране.
    if (searchQuery[0]) {
      if (weather_core::buildGeocodeUrl(searchQuery, lang, url, sizeof(url)) < 0 || !get(url, kMaxSearchBody, "поиск города")) {
        searchResult = -1;
      } else {
        searchResult = weather_core::parseGeocode(body.data(), body.size(), hits, weather_core::kMaxGeoHits);
        cal_log::line("PLACE", "поиск «%s»: %d", searchQuery, searchResult);
        for (int i = 0; i < searchResult; ++i) {
          cal_log::line("PLACE", "  %d. %s — %s (%.4f, %.4f)", i + 1, hits[i].name, hits[i].region, hits[i].lat, hits[i].lon);
        }
      }
    }

    // Место по IP — в «Авто»: давно не определяли, сменился язык или сеть (дом ⇄ работа), или только что включили «Авто».
    if (autoLocation) {
      const bool netChanged = ssidNow[0] && std::strcmp(ssidNow, ipPlace.ssid) != 0;
      const bool stale = !ipPlace.fromIp || nowEpoch < ipPlace.ipEpoch ||
                         nowEpoch - ipPlace.ipEpoch >= calendar_config::kGeoRefreshMin * 60u;
      const bool langChanged = ipPlace.fromIp && ipPlace.ipLang != static_cast<uint8_t>(lang);
      if (forceGeo || netChanged || (doWeather && (stale || langChanged))) {
        const char* why = forceGeo ? "включили авто" : netChanged ? "сменилась сеть" : langChanged ? "сменился язык" : "плановая проверка";
        weather_core::buildGeoUrl(lang, url, sizeof(url));
        weather_core::Place np = ipPlace;
        if (get(url, kMaxBody, "место по IP") && weather_core::parseGeo(body.data(), body.size(), lang, nowEpoch, np)) {
          weather_core::copyUtf8(np.ssid, sizeof(np.ssid), ssidNow);
          newPlace = np;
          geoOk = true;
          const bool moved = !weather_core::samePlace(np.lat, np.lon, target.lat, target.lon);
          cal_log::line("PLACE", "по IP (%s, сеть «%s»): %s (%.4f, %.4f)%s; ответ сервиса: %.*s", why, ssidNow, np.city, np.lat,
                        np.lon, moved ? " — место сменилось" : "", static_cast<int>(std::min<size_t>(body.size(), 220)),
                        body.data());
          if (moved) doWeather = true;  // погода нужна для нового места — прямо сейчас
          target = np;
        } else {
          cal_log::line("PLACE", "определить место по IP не удалось (%s); ответ: %.*s", why,
                        static_cast<int>(std::min<size_t>(body.size(), 220)), body.data());
        }
      }
    }

    if (doWeather && !abandoned()) {
      weather_core::buildForecastUrl(target.lat, target.lon, url, sizeof(url));
      wxTried = true;
      const bool got = get(url, kMaxBody, "погода");
      weather_core::Forecast f;
      if (got && weather_core::parseForecast(body.data(), body.size(), nowEpoch, nw, &f)) {
        nw.atLat = target.lat;
        nw.atLon = target.lon;
        wxOk = true;
        fcOk = f.valid;
        nf = f;
        cal_log::line("WX", "погода для %s: %.1f°, код %d; прогноз %u ч / %u дн.", target.city, nw.temp, nw.code,
                      static_cast<unsigned>(f.nHours), static_cast<unsigned>(f.nDays));
      } else if (got) {
        cal_log::line("WX", "погода: ответ не разобран: %.*s", static_cast<int>(std::min<size_t>(body.size(), 200)), body.data());
      }
    }

    for (unsigned i = 0; i < nYears && !abandoned(); ++i) {
      char hurl[120];
      std::snprintf(hurl, sizeof(hurl), "https://isdayoff.ru/api/getdata?year=%d&cc=%s&pre=1", years[i],
                    calendar_config::kHolidayCountry);
      char what[40];
      std::snprintf(what, sizeof(what), "календарь %d", years[i]);
      if (!get(hurl, kMaxBody, what)) {
        // Единичный сбой — пропускаем год (повторим позже); две неудачи подряд или бюджет времени — стоп.
        if (millis() - tNet >= budgetMs || failStreak >= 2) break;
        holFailed = true;
        continue;
      }
      holiday_core::YearData yd;
      const auto res = holiday_core::parseYear(body.data(), body.size(), years[i], nowEpoch, yd);
      if (res == holiday_core::ParseResult::Bad) {
        cal_log::line("HOL", "%d: сервис ответил не то", years[i]);
        holBad = true;
        continue;
      }
      hol[nHol].year = years[i];
      hol[nHol].data = yd;
      ++nHol;
      cal_log::line("HOL", "%d: %s", years[i], res == holiday_core::ParseResult::Ok ? "опубликован" : "ещё не опубликован");
    }
    netOk = failStreak < 2;
  }

  releaseWifi();
  heapInfo(mem, sizeof(mem));
  cal_log::line("NET", "готово за %lu мс%s: сеть %s, место по IP %s, погода %s; стек: свободно %u Б из %u; %s",
                static_cast<unsigned long>(millis() - t0), abandoned() ? " (календарь уже закрыт)" : "", netOk ? "ok" : "НЕТ",
                geoOk ? "ok" : "—", wxOk ? "ok" : (wxTried ? "НЕТ" : "—"),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), static_cast<unsigned>(kTaskStackBytes), mem);
}

// ---------------------------------------------------------------------------------------------------------------------
// Файлы на SD
// ---------------------------------------------------------------------------------------------------------------------

void WeatherClient::loadCache() {
  std::string body;
  {
    RenderLock lock;  // SD делит SPI с панелью
    if (!Storage.exists(kCachePath)) return;
    String s = Storage.readFile(kCachePath);
    body.assign(s.c_str(), s.length());
  }
  weather_core::Cache c;
  if (!weather_core::parseCache(body.data(), body.size(), c)) {
    cal_log::line("CACHE", "кэш погоды не читается — начинаю с нуля");
    return;
  }
  RenderLock lock;  // cache_ читает render()
  ipPlace_ = c.place;  // в файле — место по IP; показываемое место считает applyEffectivePlace()
  cache_ = c;
  cal_log::line("CACHE", "кэш: по IP %s (%s, сеть «%s»), погода %s", c.place.city, c.place.fromIp ? "определено" : "не определялось",
                c.place.ssid, c.weather.valid ? "есть" : "нет");
}

void WeatherClient::saveCache() {
  weather_core::Cache c = cache_;
  c.place = ipPlace_;  // показываемое место — производное от настроек; в кэше храним ответ геолокации
  const std::string s = weather_core::serializeCache(c);
  RenderLock lock;
  Storage.ensureDirectoryExists("/.crosspoint");
  if (!Storage.writeFile(kCachePath, String(s.c_str()))) cal_log::line("CACHE", "не удалось записать кэш погоды");
  cacheDirty_ = false;
}

void WeatherClient::loadHolidays() {
  if (!calendar_config::kHolidaysEnabled) return;
  std::string body;
  {
    RenderLock lock;
    if (!Storage.exists(kHolidaysPath)) return;
    String s = Storage.readFile(kHolidaysPath);
    body.assign(s.c_str(), s.length());
  }
  holiday_core::Store st;
  if (holiday_core::parse(body.data(), body.size(), calendar_config::kHolidayCountry, st)) {
    RenderLock lock;
    hol_ = st;
  } else {
    cal_log::line("CACHE", "кэш праздников не читается или для другой страны — не беру");
  }
}

void WeatherClient::saveHolidays() {
  const std::string s = holiday_core::serialize(hol_, calendar_config::kHolidayCountry);
  RenderLock lock;
  Storage.ensureDirectoryExists("/.crosspoint");
  if (!Storage.writeFile(kHolidaysPath, String(s.c_str()))) cal_log::line("CACHE", "не удалось записать кэш праздников");
  holDirty_ = false;
}

void WeatherClient::loadSettings() {
  std::string body;
  {
    RenderLock lock;
    if (Storage.exists(kSettingsPath)) {
      String s = Storage.readFile(kSettingsPath);
      body.assign(s.c_str(), s.length());
    }
  }
  weather_core::Settings s;  // файла нет — значения из CalendarConfig.h
  if (!body.empty() && !weather_core::parseSettings(body.data(), body.size(), s)) {
    s = weather_core::Settings{};
    LOG_ERR("WX", "calendar settings unreadable, using defaults");
  }
  {
    RenderLock lock;
    settings_ = s;
  }
  cal_log::setEnabled(s.sdLog);
  cal_log::line("CFG", "настройки: место %s, вручную %s (%.4f, %.4f), журнал на SD %s", s.autoLocation ? "авто (по IP)" : "вручную",
                s.manual.city, s.manual.lat, s.manual.lon, s.sdLog ? "вкл" : "выкл");
}

void WeatherClient::saveSettings(bool lockHeld) {
  const std::string s = weather_core::serializeSettings(settings_);
  auto write = [&] {
    Storage.ensureDirectoryExists("/.crosspoint");
    if (!Storage.writeFile(kSettingsPath, String(s.c_str()))) LOG_ERR("WX", "calendar settings write failed");
  };
  if (lockHeld) {
    write();
  } else {
    RenderLock lock;
    write();
  }
  settingsDirty_ = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Место
// ---------------------------------------------------------------------------------------------------------------------

void WeatherClient::applyEffectivePlace() {
  const weather_core::Place& p = settings_.autoLocation ? ipPlace_ : settings_.manual;
  cache_.place = p;
  const bool had = cache_.weather.valid || cache_.fc.valid;
  if (had && !weather_core::samePlace(cache_.weather.atLat, cache_.weather.atLon, p.lat, p.lon)) {
    cache_.weather = weather_core::Weather{};
    cache_.fc = weather_core::Forecast{};
    cacheDirty_ = true;
    forceRefresh_ = true;  // погода для нового места — как только будет сеть
    cal_log::line("PLACE", "показываю %s (%.4f, %.4f): погода была для другого места — загружаю заново", p.city, p.lat, p.lon);
  }
}

void WeatherClient::setAutoLocation(bool on) {
  if (settings_.autoLocation == on) return;
  settings_.autoLocation = on;
  settingsDirty_ = true;
  if (on) forceGeo_ = true;  // определить место по IP сразу, а не через kGeoRefreshMin
  cal_log::line("PLACE", "режим места: %s", on ? "авто (по IP)" : "вручную");
  applyEffectivePlace();
}

void WeatherClient::setManualPlace(const weather_core::Place& p) {
  settings_.manual = p;
  settings_.manual.fromIp = false;
  settings_.manual.ipEpoch = 0;
  settings_.manual.ssid[0] = '\0';
  settings_.autoLocation = false;
  settingsDirty_ = true;
  cal_log::line("PLACE", "место вручную: %s (%.4f, %.4f)", p.city, p.lat, p.lon);
  applyEffectivePlace();
}

void WeatherClient::pinIpPlace() {
  if (!ipPlace_.fromIp) return;
  setManualPlace(ipPlace_);
}

void WeatherClient::setSdLog(bool on) {
  if (settings_.sdLog == on) return;
  settings_.sdLog = on;
  settingsDirty_ = true;
  cal_log::setEnabled(on);
}

void WeatherClient::startSearch(const char* query) {
  weather_core::copyUtf8(searchQuery_, sizeof(searchQuery_), query);
  nHits_ = 0;
  search_ = Search::Waiting;
  searchQueued_ = true;
  cal_log::line("PLACE", "ищу город «%s»", searchQuery_);
}

void WeatherClient::pickSearchHit(int i) {
  if (i < 0 || i >= nHits_) return;
  weather_core::Place p;
  weather_core::copyUtf8(p.city, sizeof(p.city), hits_[i].name);
  p.lat = hits_[i].lat;
  p.lon = hits_[i].lon;
  clearSearch();
  setManualPlace(p);
}

void WeatherClient::clearSearch() {
  search_ = Search::Idle;
  searchQueued_ = false;
  nHits_ = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Когда выходить в сеть
// ---------------------------------------------------------------------------------------------------------------------

bool WeatherClient::weatherDue(uint32_t nowEpoch) const {
  const weather_core::Weather& w = cache_.weather;
  if (!w.valid || w.fetchedEpoch == 0) return true;
  if (nowEpoch < w.fetchedEpoch) return true;  // часы «ушли назад» — данным нельзя верить
  return nowEpoch - w.fetchedEpoch >= calendar_config::kWeatherRefreshMin * 60u;
}

// Есть ли что догрузить из производственного календаря: год, который смотрит пользователь, либо предзагрузка
// на kHolidayPrefetchYears лет вперёд (плюс перепроверка устаревших записей).
bool WeatherClient::holidayWorkPending(uint32_t nowEpoch) const {
  if (!calendar_config::kHolidaysEnabled) return false;
  if (static_cast<int32_t>(millis() - holBackoffUntilMs_) < 0) return false;
  const int cy = yearOfEpoch(nowEpoch);
  const unsigned refresh = calendar_config::kHolidayRefreshDays;
  if (wantYear_ && holiday_core::needsFetch(hol_, wantYear_, cy, nowEpoch, refresh)) return true;
  for (int y = cy; y <= cy + static_cast<int>(calendar_config::kHolidayPrefetchYears); ++y) {
    if (y <= calendar_core::kMaxYear && holiday_core::needsFetch(hol_, y, cy, nowEpoch, refresh)) return true;
  }
  return false;
}

bool WeatherClient::due(uint32_t nowEpoch) const {
  // Явная просьба пользователя (обновить, найти город, включить «Авто») — без таймеров и пауз.
  if (forceRefresh_ || searchQueued_ || forceGeo_) return true;
  if (static_cast<int32_t>(millis() - nextTryMs_) < 0) return false;
  // Тихий период: пока идёт ввод — не начинаем (не нагружаем Wi-Fi/процессор посреди листания).
  const uint32_t needIdleMs = detailOpen_ ? calendar_config::kDetailNetworkIdleSec * 1000u : calendar_config::kOnDemandDebounceMs;
  if (idleMs_ < needIdleMs) return false;
  return weatherDue(nowEpoch) || holidayWorkPending(nowEpoch);
}

bool WeatherClient::step(GfxRenderer* renderer, uint32_t nowEpoch, calendar_core::Lang lang) {
  bool changed = false;
  if (!loaded_) {
    loaded_ = true;
    nextTryMs_ = millis() + kFirstDelayMs;
    loadSettings();
    loadCache();
    loadHolidays();
    {
      RenderLock lock;
      applyEffectivePlace();
    }
    changed = true;
  }
  if (settingsDirty_) saveSettings();
  if (cacheDirty_ && !job_) saveCache();  // место сменили с экрана «Место»: погода сброшена — сохранить это
  if (!nowEpoch) return changed;

  if (job_) {  // задача идёт: смотрим только флаг готовности
    if (job_->state.load(std::memory_order_acquire) == 1) {
      Job* j = job_;
      job_ = nullptr;
      applyJob(*j, nowEpoch);
      delete j;
      return true;
    }
    return changed;
  }

  if (!renderer) return changed;
  const bool urgent = forceRefresh_ || searchQueued_ || forceGeo_;
  // «Пора ли» — не на каждом такте loop() (их сотни в секунду), а раз в секунду; просьба пользователя — сразу.
  const uint32_t now = millis();
  if (!urgent && now - lastDueCheckMs_ < kDueCheckEveryMs) return changed;
  lastDueCheckMs_ = now;
  if (due(nowEpoch)) startJob(*renderer, nowEpoch, lang);
  return changed;
}

void WeatherClient::startJob(GfxRenderer& renderer, uint32_t nowEpoch, calendar_core::Lang lang) {
  if (g_netBusy.load()) {  // брошенная задача ещё не доработала
    nextTryMs_ = millis() + 2000;
    return;
  }
  const bool urgent = forceRefresh_ || searchQueued_ || forceGeo_;

  // Мало заряда — плановые запросы пропускаем (явную просьбу — выполняем).
  const int battery = powerManager.getBatteryPercentage();
  if (!urgent && battery < static_cast<int>(calendar_config::kMinBatteryPctForNetwork)) {
    cal_log::line("NET", "заряд %d%% — плановый выход в сеть пропущен", battery);
    nextTryMs_ = millis() + kLowBatteryRetryMs;
    return;
  }

  auto up = makeUniqueNoThrow<Job>();
  if (!up) {
    LOG_ERR("WX", "OOM: network job");
    cal_log::line("NET", "нет памяти под сетевую задачу");
    nextTryMs_ = millis() + kFailRetryMs;
    return;
  }

  // Wi-Fi уже подключён кем-то и правда работает (радио включено, есть адрес) — пользуемся, не выключаем.
  // Один статус не годится: после выключения Wi-Fi чужим кодом он бывает «подключено» до перезагрузки.
  const bool radioOn = WiFi.getMode() != WIFI_MODE_NULL;
  if (radioOn && WiFi.status() == WL_CONNECTED && hasAddress()) {
    up->alreadyConnected = true;
    busySinceMs_ = 0;
  } else {
    if (radioOn) {
      // Wi-Fi включён кем-то другим (синхронизация времени и т.п.) — не мешаем. Но если он так и висит не подключённым
      // дольше kWifiBusyTakeoverSec — его бросили: берём себе, иначе погода не обновилась бы до перезагрузки.
      const uint32_t now = millis();
      if (!busySinceMs_) busySinceMs_ = now ? now : 1;
      if (now - busySinceMs_ < kBusyTakeoverMs) {
        cal_log::line("NET", "Wi-Fi занят (режим %d, статус %d) — жду", static_cast<int>(WiFi.getMode()), static_cast<int>(WiFi.status()));
        nextTryMs_ = now + kBusyRetryMs;
        return;
      }
      cal_log::line("NET", "Wi-Fi занят уже %lu с и так и не подключился — забираю", static_cast<unsigned long>((now - busySinceMs_) / 1000));
    }
    busySinceMs_ = 0;
    RenderLock lock;  // файл с сетями лежит на SD
    if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
    const size_t n = std::min<size_t>(WIFI_STORE.getCredentialCount(), Job::kMaxCandidates);
    for (size_t i = 0; i < n; ++i) {
      if (const auto cred = WIFI_STORE.getCredentialAt(i)) up->candidates[up->nCandidates++] = {cred->ssid, cred->password};
    }
    up->lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (up->nCandidates == 0) {
      cal_log::line("NET", "в устройстве нет сохранённых Wi-Fi-сетей — погода остаётся из кэша");
      forceRefresh_ = false;
      forceGeo_ = false;
      if (searchQueued_) {
        searchQueued_ = false;
        search_ = Search::Failed;
      }
      nextTryMs_ = millis() + kNoCredsRetryMs;
      return;
    }
  }

  // Что делать: погода (если пора или просили), место по IP (решит задача: зависит от сети), годы календаря, поиск.
  Job& j = *up;
  j.renderer = &renderer;
  j.nowEpoch = nowEpoch;
  j.lang = lang;
  j.autoLocation = settings_.autoLocation;
  j.forceGeo = forceGeo_ && settings_.autoLocation;
  j.ipPlace = ipPlace_;
  j.target = cache_.place;
  j.doWeather = forceRefresh_ || weatherDue(nowEpoch);
  if (searchQueued_) weather_core::copyUtf8(j.searchQuery, sizeof(j.searchQuery), searchQuery_);
  if (calendar_config::kHolidaysEnabled && static_cast<int32_t>(millis() - holBackoffUntilMs_) >= 0) {
    const int cy = yearOfEpoch(nowEpoch);
    const unsigned refresh = calendar_config::kHolidayRefreshDays;
    auto add = [&](int y) {
      if (y < calendar_core::kMinYear || y > calendar_core::kMaxYear || j.nYears >= Job::kMaxYearsPerJob) return;
      for (unsigned i = 0; i < j.nYears; ++i) {
        if (j.years[i] == y) return;
      }
      if (holiday_core::needsFetch(hol_, y, cy, nowEpoch, refresh)) j.years[j.nYears++] = y;
    };
    add(wantYear_);  // год, который смотрит пользователь, — первым
    for (int y = cy; y <= cy + static_cast<int>(calendar_config::kHolidayPrefetchYears); ++y) add(y);
  }
  if (!j.doWeather && j.nYears == 0 && !j.searchQuery[0] && !j.forceGeo) {  // делать нечего
    forceRefresh_ = false;
    forceGeo_ = false;
    return;
  }

  const bool weatherJob = j.doWeather;
  g_netBusy.store(true);
  Job* raw = up.release();
  const BaseType_t created = xTaskCreate(
      [](void* arg) {
        Job* job = static_cast<Job*>(arg);
        job->run();
        int expected = 0;
        // Грань ждёт результат (0→1) — данные теперь её. Если она закрылась раньше (2) — удаляем сами.
        // После этой строки к job обращаться нельзя: им уже может владеть грань.
        if (!job->state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) delete job;
        g_netBusy.store(false);
#if CROSSPOINT_EMULATED == 0
        vTaskDelete(nullptr);  // задача FreeRTOS не должна «возвращаться»; в симуляторе это обычный поток — он просто завершится
#endif
      },
      "CalNet", kTaskStackBytes, raw, 1, nullptr);
  if (!created) {
    cal_log::line("NET", "не удалось запустить сетевую задачу (стек %u Б)", static_cast<unsigned>(kTaskStackBytes));
    g_netBusy.store(false);
    delete raw;
    nextTryMs_ = millis() + kFailRetryMs;
    return;
  }
  job_ = raw;
  if (weatherJob) forceRefresh_ = false;
  forceGeo_ = false;
  searchQueued_ = false;
}

// Результат задачи — в данные грани. Присваивания — под RenderLock: render() читает эти структуры из другой задачи.
void WeatherClient::applyJob(Job& j, uint32_t nowEpoch) {
  const int cy = yearOfEpoch(nowEpoch);
  {
    RenderLock lock;
    if (j.geoOk) {
      ipPlace_ = j.newPlace;
      cacheDirty_ = true;
      if (settings_.autoLocation) applyEffectivePlace();  // место сменилось — старая погода сброшена
    }
    if (j.wxOk) {
      // Погода для того места, что показывается сейчас (пока шёл запрос, режим или город могли переключить).
      if (weather_core::samePlace(j.nw.atLat, j.nw.atLon, cache_.place.lat, cache_.place.lon)) {
        cache_.weather = j.nw;
        if (j.fcOk) cache_.fc = j.nf;
        cacheDirty_ = true;
        forceRefresh_ = false;  // свежая погода именно для этого места уже есть
      } else {
        cal_log::line("WX", "погода получена для прежнего места — не показываю (место сменили, пока шёл запрос)");
      }
    }
    for (unsigned i = 0; i < j.nHol; ++i) {
      if (holiday_core::YearData* slot = hol_.put(j.hol[i].year, cy)) *slot = j.hol[i].data;
      holDirty_ = true;
    }
    // Подключились не к «последней использованной» сети — запомнить новую: и наши будущие циклы, и штатная
    // тихая синхронизация времени (StandbyActivity::trySilentWifiConnect) читают именно это поле.
    if (!j.ourSsid.empty()) WIFI_STORE.setLastConnectedSsid(j.ourSsid);
    if (j.searchQuery[0] && search_ == Search::Waiting && std::strcmp(j.searchQuery, searchQuery_) == 0) {
      if (j.searchResult < 0) {
        search_ = Search::Failed;  // -2: до поиска не дошло (нет сети), -1: сервис не ответил
      } else if (j.searchResult == 0) {
        search_ = Search::NotFound;
      } else {
        nHits_ = j.searchResult;
        std::copy(j.hits, j.hits + nHits_, hits_);
        search_ = Search::Found;
        if (nHits_ == 1) pickSearchHit(0);  // единственный вариант — выбираем сразу
      }
    }
  }
  if (cacheDirty_) saveCache();
  if (holDirty_) saveHolidays();
  if (settingsDirty_) saveSettings();

  const bool wxFailed = j.wxTried && !j.wxOk;
  nextTryMs_ = millis() + ((wxFailed || !j.netOk) ? kFailRetryMs : 0u);
  if (j.holBad || j.holFailed || !j.netOk) holBackoffUntilMs_ = millis() + kFailRetryMs;
  if (wxFailed || !j.netOk) {
    cal_log::line("NET", "следующая попытка через %u мин", static_cast<unsigned>(calendar_config::kRetryAfterFailMin));
  }
  cal_log::pump(true);  // итог выхода в сеть — сразу на карту
}

void WeatherClient::stop(bool lockHeld) {
  if (job_) {
    int expected = 0;
    // Задача ещё работает (0→2): она сама быстро закончит и удалит. Уже готова (1) — удаляем мы.
    if (!job_->state.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) delete job_;
    job_ = nullptr;
  }
  if (settingsDirty_) saveSettings(lockHeld);  // переключили на экране «Место» и сразу вышли
  loaded_ = false;  // при следующем входе в грань перечитаем кэш и настройки
}
