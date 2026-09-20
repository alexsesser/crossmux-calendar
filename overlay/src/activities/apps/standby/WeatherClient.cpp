#include "WeatherClient.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

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
constexpr size_t kMaxBody = 4096;  // ответы — сотни байт; больше — явно не то (не настройка)

bool fetch(const char* url, std::string& out) {
  if (!HttpDownloader::fetchUrl(url, out)) return false;
  return !out.empty() && out.size() <= kMaxBody;
}

}  // namespace

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

namespace {
int yearOfEpoch(uint32_t epoch) {
  int y;
  unsigned m, d;
  calendar_core::civilFromDays(static_cast<int32_t>(epoch / 86400u), y, m, d);
  return y;
}
}  // namespace

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
  if (static_cast<int32_t>(millis() - nextTryMs_) < 0) return false;
  // Тихий период: пока идёт ввод — не начинаем (запрос блокирует цикл на пару секунд).
  const uint32_t needIdleMs = detailOpen_ ? calendar_config::kDetailNetworkIdleSec * 1000u : calendar_config::kOnDemandDebounceMs;
  if (idleMs_ < needIdleMs) return false;
  return forceRefresh_ || weatherDue(nowEpoch) || holidayWorkPending(nowEpoch);
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
  if (!renderer || !nowEpoch) return changed;

  if (phase_ == Phase::Idle) {
    if (due(nowEpoch)) startCycle(*renderer);
    return changed;
  }

  // Phase::Connecting
  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    const bool ok = fetchAll(nowEpoch, lang);
    finish(ok);
    return true;
  }
  const bool hardFail = st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL;
  if (hardFail || millis() - phaseStartMs_ >= kConnectTimeoutMs) {
    LOG_DBG("WX", "wifi connect failed (status=%d)", static_cast<int>(st));
    finish(false);
  }
  return changed;
}

void WeatherClient::startCycle(GfxRenderer& renderer) {
  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {  // кто-то уже подключил — пользуемся, но не выключаем
    ownsWifi_ = false;
    phase_ = Phase::Connecting;
    phaseStartMs_ = millis();
    return;
  }
  if (WiFi.getMode() != WIFI_MODE_NULL) {  // Wi-Fi занят (синхронизация времени и т.п.) — не мешаем
    nextTryMs_ = millis() + kBusyRetryMs;
    return;
  }

  std::string ssid, pass;
  {
    RenderLock lock;
    if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
    const std::string last = WIFI_STORE.getLastConnectedSsid();
    const auto cred = last.empty() ? std::nullopt : WIFI_STORE.findCredential(last);
    if (!cred) {
      LOG_DBG("WX", "no saved wifi network; weather stays cached");
      nextTryMs_ = millis() + kNoCredsRetryMs;
      return;
    }
    ssid = cred->ssid;
    pass = cred->password;
  }

  WiFi.persistent(false);
  if (!NetworkStartup::setMode(renderer, WIFI_STA)) {
    LOG_ERR("WX", "wifi mode failed");
    nextTryMs_ = millis() + kFailRetryMs;
    return;
  }
  WiFi.disconnect(true, true);
  delay(100);
  if (pass.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
  ownsWifi_ = true;
  phase_ = Phase::Connecting;
  phaseStartMs_ = millis();
  LOG_DBG("WX", "wifi connecting: %s", ssid.c_str());
}

// Сначала место по IP (не чаще раза в kGeoRefreshMin и при смене языка), потом погода для этого места (текущая +
// 24 часа + 7 дней одним запросом), потом — производственный календарь (годы, которых нет в кэше).
// Неудача геолокации не мешает погоде: берём запомненное место (по умолчанию — Москва).
bool WeatherClient::fetchAll(uint32_t nowEpoch, calendar_core::Lang lang) {
  weather_core::Place& p = cache_.place;
  char url[700];  // URL прогноза ≈ 450 символов
  std::string body;
  bool ok = true;

  if (forceRefresh_ || weatherDue(nowEpoch)) {
    forceRefresh_ = false;
    const bool needGeo = !p.fromIp || p.ipLang != static_cast<uint8_t>(lang) || nowEpoch < p.ipEpoch ||
                         nowEpoch - p.ipEpoch >= calendar_config::kGeoRefreshMin * 60u;
    if (needGeo) {
      weather_core::buildGeoUrl(lang, url, sizeof(url));
      weather_core::Place np = p;
      if (fetch(url, body) && weather_core::parseGeo(body.data(), body.size(), lang, nowEpoch, np)) {
        LOG_DBG("WX", "place by IP: %s (%.3f, %.3f)", np.city, np.lat, np.lon);
        p = np;
        cacheDirty_ = true;
      } else {
        LOG_DBG("WX", "IP geolocation failed; keeping %s", p.city);
      }
    }

    weather_core::buildForecastUrl(p.lat, p.lon, url, sizeof(url));
    weather_core::Weather nw;
    weather_core::Forecast nf;
    const bool got = fetch(url, body);
    ok = got && weather_core::parseForecast(body.data(), body.size(), nowEpoch, nw);
    if (!ok) LOG_DBG("WX", "forecast: %s (%u bytes)", got ? "parse failed" : "download failed", static_cast<unsigned>(body.size()));
    if (ok) {
      cache_.weather = nw;
      if (weather_core::parseForecastDetail(body.data(), body.size(), nowEpoch, nf)) cache_.fc = nf;
      cacheDirty_ = true;
      LOG_DBG("WX", "weather ok: %.1f code=%d, forecast %uh/%ud", nw.temp, nw.code, static_cast<unsigned>(nf.nHours),
              static_cast<unsigned>(nf.nDays));
    } else {
      LOG_DBG("WX", "forecast failed");
    }
    if (cacheDirty_) saveCache();
  }

  if (!fetchHolidays(nowEpoch)) LOG_DBG("WX", "holidays: some requests failed");
  return ok;
}

// Годы производственного календаря: сперва тот, что смотрит пользователь, затем предзагрузка. Не больше
// kHolidayMaxRequestsPerCycle запросов за выход в сеть — остальное следующими циклами.
bool WeatherClient::fetchHolidays(uint32_t nowEpoch) {
  if (!calendar_config::kHolidaysEnabled) return true;
  const int cy = yearOfEpoch(nowEpoch);
  const unsigned refresh = calendar_config::kHolidayRefreshDays;
  int years[calendar_config::kHolidayMaxRequestsPerCycle + 2];
  unsigned n = 0;
  auto add = [&](int y) {
    if (y < calendar_core::kMinYear || y > calendar_core::kMaxYear || n >= calendar_config::kHolidayMaxRequestsPerCycle) return;
    for (unsigned i = 0; i < n; ++i) if (years[i] == y) return;
    if (holiday_core::needsFetch(hol_, y, cy, nowEpoch, refresh)) years[n++] = y;
  };
  add(wantYear_);
  for (int y = cy; y <= cy + static_cast<int>(calendar_config::kHolidayPrefetchYears); ++y) add(y);

  bool allOk = true;
  std::string body;
  for (unsigned i = 0; i < n; ++i) {
    char url[120];
    std::snprintf(url, sizeof(url), "https://isdayoff.ru/api/getdata?year=%d&cc=%s&pre=1", years[i],
                  calendar_config::kHolidayCountry);
    if (!fetch(url, body)) {
      allOk = false;
      break;  // сеть/сервис недоступны — остальные годы не пробуем
    }
    holiday_core::YearData yd;
    const auto res = holiday_core::parseYear(body.data(), body.size(), years[i], nowEpoch, yd);
    if (res == holiday_core::ParseResult::Bad) {
      LOG_DBG("WX", "holidays %d: bad answer", years[i]);
      allOk = false;
      continue;
    }
    if (holiday_core::YearData* slot = hol_.put(years[i], cy)) *slot = yd;
    holDirty_ = true;
    LOG_DBG("WX", "holidays %d: %s", years[i], res == holiday_core::ParseResult::Ok ? "published" : "not published yet");
  }
  if (holDirty_) saveHolidays();
  if (!allOk) holBackoffUntilMs_ = millis() + calendar_config::kRetryAfterFailMin * 60u * 1000u;
  return allOk;
}

void WeatherClient::finish(bool ok) {
  teardownWifi();
  phase_ = Phase::Idle;
  nextTryMs_ = millis() + (ok ? 0u : kFailRetryMs);
}

void WeatherClient::teardownWifi() {
  if (!ownsWifi_) return;
  ownsWifi_ = false;
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  esp_wifi_deinit();  // как StandbyActivity::stopTimeSyncWifi: без этого CPU не опускается на LOW_POWER_FREQ
}

void WeatherClient::stop() {
  if (phase_ == Phase::Connecting) finish(false);
  loaded_ = false;  // при следующем входе в грань перечитаем кэш
}
