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

#include <atomic>
#include <string>

#include "CalendarConfig.h"
#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "activities/RenderLock.h"
#include "network/HttpDownloader.h"

namespace {

constexpr const char* kCachePath = "/.crosspoint/calendar_cache.json";
constexpr const char* kHolidaysPath = "/.crosspoint/calendar_holidays.json";

// Тайминги — в CalendarConfig.h.
constexpr uint32_t kFirstDelayMs = calendar_config::kFirstRequestDelaySec * 1000u;
constexpr uint32_t kConnectTimeoutMs = calendar_config::kWifiConnectTimeoutSec * 1000u;
constexpr uint32_t kBusyRetryMs = calendar_config::kRetryWifiBusySec * 1000u;
constexpr uint32_t kFailRetryMs = calendar_config::kRetryAfterFailMin * 60u * 1000u;
constexpr uint32_t kNoCredsRetryMs = calendar_config::kRetryNoWifiMin * 60u * 1000u;
constexpr uint32_t kLowBatteryRetryMs = calendar_config::kRetryLowBatteryMin * 60u * 1000u;
constexpr size_t kMaxBody = 4096;                // ответы — сотни байт; больше — явно не то (не настройка)
constexpr uint32_t kTaskStackBytes = 10240;      // TLS-рукопожатие + разбор JSON; у upstream для таких задач 8–16 КБ (не настройка)
constexpr uint32_t kDueCheckEveryMs = 1000;

// Одна сетевая задача за раз на всё устройство: задача, брошенная закрытой гранью, ещё может работать.
std::atomic<bool> g_netBusy{false};

int yearOfEpoch(uint32_t epoch) {
  int y;
  unsigned m, d;
  calendar_core::civilFromDays(static_cast<int32_t>(epoch / 86400u), y, m, d);
  return y;
}

bool fetch(const char* url, std::string& out) {
  if (!HttpDownloader::fetchUrl(url, out)) return false;
  return !out.empty() && out.size() <= kMaxBody;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Задача: вход → результат. Принадлежит либо задаче (пока работает), либо грани (после «готово»), либо снова задаче
// (если грань закрылась раньше): кто последним «отпустил» — тот и удаляет. Состояние решает атомарный CAS.
// ---------------------------------------------------------------------------------------------------------------------
struct WeatherClient::Job {
  static constexpr unsigned kMaxYearsPerJob = calendar_config::kHolidayMaxRequestsPerCycle;

  // Вход.
  GfxRenderer* renderer = nullptr;
  uint32_t nowEpoch = 0;
  calendar_core::Lang lang = calendar_core::Lang::En;
  bool alreadyConnected = false;  // Wi-Fi поднят не нами — не выключаем
  std::string ssid, pass;
  bool doWeather = false;
  bool needGeo = false;
  weather_core::Place place;
  unsigned nYears = 0;
  int years[kMaxYearsPerJob] = {};

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

  // 0 — работает; 1 — готово, владелец грань; 2 — брошено гранью, владелец задача.
  std::atomic<int> state{0};

  void run();
};

void WeatherClient::Job::run() {
  const uint32_t t0 = millis();
  const uint32_t budgetMs = calendar_config::kNetworkBudgetSec * 1000u;
  bool ownsWifi = false;

  if (!alreadyConnected) {
    WiFi.persistent(false);
    if (!NetworkStartup::setMode(*renderer, WIFI_STA)) {
      LOG_ERR("WX", "wifi mode failed");
      netOk = false;
      return;
    }
    WiFi.disconnect(true, true);  // «чистый лист», как в StandbyActivity::trySilentWifiConnect
    delay(100);
    if (pass.empty()) {
      WiFi.begin(ssid.c_str());
    } else {
      WiFi.begin(ssid.c_str(), pass.c_str());
    }
    ownsWifi = true;
    LOG_DBG("WX", "wifi connecting: %s", ssid.c_str());
    wl_status_t st;
    do {
      delay(100);
      st = WiFi.status();
    } while (st != WL_CONNECTED && st != WL_CONNECT_FAILED && st != WL_NO_SSID_AVAIL && millis() - t0 < kConnectTimeoutMs);
    if (st != WL_CONNECTED) {
      LOG_DBG("WX", "wifi connect failed (status=%d)", static_cast<int>(st));
      netOk = false;
    }
  }

  if (netOk) {
    std::string body;
    char url[700];  // URL прогноза ≈ 450 символов
    int failStreak = 0;
    // Запрос идёт, только пока есть бюджет времени и сеть не «легла» (два подряд провала).
    auto get = [&](const char* u) {
      if (millis() - t0 >= budgetMs || failStreak >= 2) return false;
      const bool ok = fetch(u, body);
      failStreak = ok ? 0 : failStreak + 1;
      return ok;
    };

    if (needGeo) {
      weather_core::buildGeoUrl(lang, url, sizeof(url));
      weather_core::Place np = place;
      if (get(url) && weather_core::parseGeo(body.data(), body.size(), lang, nowEpoch, np)) {
        newPlace = np;
        geoOk = true;
        LOG_DBG("WX", "place by IP: %s (%.3f, %.3f)", np.city, np.lat, np.lon);
      } else {
        LOG_DBG("WX", "IP geolocation failed; keeping %s", place.city);
      }
    }

    if (doWeather) {
      const weather_core::Place& p = geoOk ? newPlace : place;
      weather_core::buildForecastUrl(p.lat, p.lon, url, sizeof(url));
      wxTried = true;
      const bool got = get(url);
      weather_core::Forecast f;
      if (got && weather_core::parseForecast(body.data(), body.size(), nowEpoch, nw, &f)) {
        wxOk = true;
        fcOk = f.valid;
        nf = f;
        LOG_DBG("WX", "weather ok: %.1f code=%d, forecast %uh/%ud", nw.temp, nw.code, static_cast<unsigned>(f.nHours),
                static_cast<unsigned>(f.nDays));
      } else {
        LOG_DBG("WX", "forecast: %s (%u bytes)", got ? "parse failed" : "download failed", static_cast<unsigned>(body.size()));
      }
    }

    for (unsigned i = 0; i < nYears; ++i) {
      char hurl[120];
      std::snprintf(hurl, sizeof(hurl), "https://isdayoff.ru/api/getdata?year=%d&cc=%s&pre=1", years[i],
                    calendar_config::kHolidayCountry);
      if (!get(hurl)) {
        // Единичный сбой — пропускаем год (повторим позже); две неудачи подряд или бюджет времени — стоп.
        if (millis() - t0 >= budgetMs || failStreak >= 2) break;
        holFailed = true;
        continue;
      }
      holiday_core::YearData yd;
      const auto res = holiday_core::parseYear(body.data(), body.size(), years[i], nowEpoch, yd);
      if (res == holiday_core::ParseResult::Bad) {
        LOG_DBG("WX", "holidays %d: bad answer", years[i]);
        holBad = true;
        continue;
      }
      hol[nHol].year = years[i];
      hol[nHol].data = yd;
      ++nHol;
      LOG_DBG("WX", "holidays %d: %s", years[i], res == holiday_core::ParseResult::Ok ? "published" : "not published yet");
    }
    netOk = failStreak < 2;
  }

  if (ownsWifi) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    esp_wifi_deinit();  // как StandbyActivity::stopTimeSyncWifi: без этого CPU не опускается на LOW_POWER_FREQ
  }
}

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
  if (weather_core::parseCache(body.data(), body.size(), c)) {
    cache_ = c;
    LOG_DBG("WX", "cache loaded: %s fromIp=%d wx=%d", cache_.place.city, cache_.place.fromIp, cache_.weather.valid);
  } else {
    LOG_ERR("WX", "cache unreadable, using defaults");
  }
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
    hol_ = st;
    LOG_DBG("WX", "holidays cache loaded: %u years", static_cast<unsigned>(hol_.count));
  } else {
    LOG_ERR("WX", "holidays cache unreadable/foreign country, ignoring");
  }
}

void WeatherClient::saveHolidays() {
  const std::string s = holiday_core::serialize(hol_, calendar_config::kHolidayCountry);
  RenderLock lock;
  Storage.ensureDirectoryExists("/.crosspoint");
  if (!Storage.writeFile(kHolidaysPath, String(s.c_str()))) LOG_ERR("WX", "holidays cache write failed");
  holDirty_ = false;
}

void WeatherClient::saveCache() {
  const std::string s = weather_core::serializeCache(cache_);
  RenderLock lock;
  Storage.ensureDirectoryExists("/.crosspoint");
  if (!Storage.writeFile(kCachePath, String(s.c_str()))) LOG_ERR("WX", "cache write failed");
  cacheDirty_ = false;
}

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
  if (forceRefresh_) return true;  // явная просьба пользователя — без таймеров и пауз
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
    loadCache();
    loadHolidays();
    changed = true;
  }
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
  // «Пора ли» — не на каждом такте loop() (их сотни в секунду), а раз в секунду; просьба пользователя — сразу.
  const uint32_t now = millis();
  if (!forceRefresh_ && now - lastDueCheckMs_ < kDueCheckEveryMs) return changed;
  lastDueCheckMs_ = now;
  if (due(nowEpoch)) startJob(*renderer, nowEpoch, lang);
  return changed;
}

void WeatherClient::startJob(GfxRenderer& renderer, uint32_t nowEpoch, calendar_core::Lang lang) {
  if (g_netBusy.load()) {  // брошенная задача ещё не доработала
    nextTryMs_ = millis() + 2000;
    return;
  }
  const bool forced = forceRefresh_;

  // Мало заряда — плановые запросы пропускаем (явную просьбу — выполняем).
  if (!forced && powerManager.getBatteryPercentage() < calendar_config::kMinBatteryPctForNetwork) {
    LOG_DBG("WX", "battery low: skipping network");
    nextTryMs_ = millis() + kLowBatteryRetryMs;
    return;
  }

  auto up = makeUniqueNoThrow<Job>();
  if (!up) {
    LOG_ERR("WX", "OOM: network job");
    nextTryMs_ = millis() + kFailRetryMs;
    return;
  }

  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {  // кто-то уже подключил — пользуемся, но не выключаем
    up->alreadyConnected = true;
  } else {
    if (WiFi.getMode() != WIFI_MODE_NULL) {  // Wi-Fi занят (синхронизация времени и т.п.) — не мешаем
      nextTryMs_ = millis() + kBusyRetryMs;
      return;
    }
    RenderLock lock;  // файл с сетями лежит на SD
    if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
    const std::string last = WIFI_STORE.getLastConnectedSsid();
    const auto cred = last.empty() ? std::nullopt : WIFI_STORE.findCredential(last);
    if (!cred) {
      LOG_DBG("WX", "no saved wifi network; weather stays cached");
      forceRefresh_ = false;
      nextTryMs_ = millis() + kNoCredsRetryMs;
      return;
    }
    up->ssid = cred->ssid;
    up->pass = cred->password;
  }

  // Что качать: погода (если пора или просили), место по IP (если давно/сменился язык), годы календаря.
  Job& j = *up;
  j.renderer = &renderer;
  j.nowEpoch = nowEpoch;
  j.lang = lang;
  j.place = cache_.place;
  j.doWeather = forced || weatherDue(nowEpoch);
  j.needGeo = j.doWeather && (!j.place.fromIp || j.place.ipLang != static_cast<uint8_t>(lang) || nowEpoch < j.place.ipEpoch ||
                              nowEpoch - j.place.ipEpoch >= calendar_config::kGeoRefreshMin * 60u);
  if (calendar_config::kHolidaysEnabled && static_cast<int32_t>(millis() - holBackoffUntilMs_) >= 0) {
    const int cy = yearOfEpoch(nowEpoch);
    const unsigned refresh = calendar_config::kHolidayRefreshDays;
    auto add = [&](int y) {
      if (y < calendar_core::kMinYear || y > calendar_core::kMaxYear || j.nYears >= Job::kMaxYearsPerJob) return;
      for (unsigned i = 0; i < j.nYears; ++i) if (j.years[i] == y) return;
      if (holiday_core::needsFetch(hol_, y, cy, nowEpoch, refresh)) j.years[j.nYears++] = y;
    };
    add(wantYear_);  // год, который смотрит пользователь, — первым
    for (int y = cy; y <= cy + static_cast<int>(calendar_config::kHolidayPrefetchYears); ++y) add(y);
  }
  if (!j.doWeather && j.nYears == 0) {  // делать нечего
    forceRefresh_ = false;
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
    LOG_ERR("WX", "cannot start network task");
    g_netBusy.store(false);
    delete raw;
    nextTryMs_ = millis() + kFailRetryMs;
    return;
  }
  job_ = raw;
  if (weatherJob) forceRefresh_ = false;
}

// Результат задачи — в данные грани. Присваивания — под RenderLock: render() читает эти структуры из другой задачи.
void WeatherClient::applyJob(Job& j, uint32_t nowEpoch) {
  const int cy = yearOfEpoch(nowEpoch);
  {
    RenderLock lock;
    if (j.geoOk) {
      cache_.place = j.newPlace;
      cacheDirty_ = true;
    }
    if (j.wxOk) {
      cache_.weather = j.nw;
      if (j.fcOk) cache_.fc = j.nf;
      cacheDirty_ = true;
    }
    for (unsigned i = 0; i < j.nHol; ++i) {
      if (holiday_core::YearData* slot = hol_.put(j.hol[i].year, cy)) *slot = j.hol[i].data;
      holDirty_ = true;
    }
  }
  if (cacheDirty_) saveCache();
  if (holDirty_) saveHolidays();

  const bool wxFailed = j.wxTried && !j.wxOk;
  nextTryMs_ = millis() + ((wxFailed || !j.netOk) ? kFailRetryMs : 0u);
  if (j.holBad || j.holFailed || !j.netOk) holBackoffUntilMs_ = millis() + kFailRetryMs;
}

void WeatherClient::stop() {
  if (job_) {
    int expected = 0;
    // Задача ещё работает (0→2): она сама всё доделает и удалит. Уже готова (1) — удаляем мы.
    if (!job_->state.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) delete job_;
    job_ = nullptr;
  }
  loaded_ = false;  // при следующем входе в грань перечитаем кэш
}
