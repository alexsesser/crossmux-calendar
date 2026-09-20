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
constexpr uint32_t kFirstDelayMs = 4000;        // дать сначала нарисоваться экрану
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kBusyRetryMs = 60u * 1000u;  // Wi-Fi занят кем-то ещё
constexpr uint32_t kFailRetryMs = 5u * 60u * 1000u;
constexpr uint32_t kNoCredsRetryMs = 30u * 60u * 1000u;
constexpr size_t kMaxBody = 4096;                // ответы — сотни байт; больше — явно не то

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

void WeatherClient::saveCache() {
  const std::string s = weather_core::serializeCache(cache_);
  RenderLock lock;
  Storage.ensureDirectoryExists("/.crosspoint");
  if (!Storage.writeFile(kCachePath, String(s.c_str()))) LOG_ERR("WX", "cache write failed");
  cacheDirty_ = false;
}

bool WeatherClient::due(uint32_t nowEpoch) const {
  if (static_cast<int32_t>(millis() - nextTryMs_) < 0) return false;
  const weather_core::Weather& w = cache_.weather;
  if (!w.valid || w.fetchedEpoch == 0) return true;
  if (nowEpoch < w.fetchedEpoch) return true;  // часы «ушли назад» — данным нельзя верить
  return nowEpoch - w.fetchedEpoch >= calendar_config::kWeatherRefreshMin * 60u;
}

bool WeatherClient::step(GfxRenderer* renderer, uint32_t nowEpoch, calendar_core::Lang lang) {
  bool changed = false;
  if (!loaded_) {
    loaded_ = true;
    nextTryMs_ = millis() + kFirstDelayMs;
    loadCache();
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

// Сначала место по IP (не чаще раза в kGeoRefreshMin и при смене языка), потом погода для этого места.
// Неудача геолокации не мешает погоде: берём запомненное место (по умолчанию — Москва).
bool WeatherClient::fetchAll(uint32_t nowEpoch, calendar_core::Lang lang) {
  weather_core::Place& p = cache_.place;
  const bool needGeo = !p.fromIp || p.ipLang != static_cast<uint8_t>(lang) || nowEpoch < p.ipEpoch ||
                       nowEpoch - p.ipEpoch >= calendar_config::kGeoRefreshMin * 60u;
  char url[420];
  std::string body;

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
  const bool ok = fetch(url, body) && weather_core::parseForecast(body.data(), body.size(), nowEpoch, nw);
  if (ok) {
    cache_.weather = nw;
    cacheDirty_ = true;
    LOG_DBG("WX", "weather ok: %.1f code=%d", nw.temp, nw.code);
  } else {
    LOG_DBG("WX", "forecast failed");
  }
  if (cacheDirty_) saveCache();
  return ok;
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
