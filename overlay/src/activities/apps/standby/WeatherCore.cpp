#include "WeatherCore.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

namespace weather_core {

using calendar_core::Lang;

// ---- Таблица кодов WMO (Open-Meteo) ------------------------------------------

namespace {

struct Entry {
  int16_t code;
  Icon icon;
  const char* en;
  const char* ru;
  const char* de;
};

const Entry kTable[] = {
    {0, Icon::Clear, "Clear", "Ясно", "Klar"},
    {1, Icon::Clear, "Mostly clear", "Малооблачно", "Heiter"},
    {2, Icon::PartlyCloudy, "Partly cloudy", "Облачно", "Wolkig"},
    {3, Icon::Cloudy, "Overcast", "Пасмурно", "Bedeckt"},
    {45, Icon::Fog, "Fog", "Туман", "Nebel"},
    {48, Icon::Fog, "Rime fog", "Изморозь", "Reifnebel"},
    {51, Icon::Drizzle, "Light drizzle", "Слабая морось", "Leichter Niesel"},
    {53, Icon::Drizzle, "Drizzle", "Морось", "Nieselregen"},
    {55, Icon::Drizzle, "Heavy drizzle", "Сильная морось", "Starker Niesel"},
    {56, Icon::Drizzle, "Freezing drizzle", "Ледяная морось", "Gefr. Niesel"},
    {57, Icon::Drizzle, "Freezing drizzle", "Ледяная морось", "Gefr. Niesel"},
    {61, Icon::Rain, "Light rain", "Небольшой дождь", "Leichter Regen"},
    {63, Icon::Rain, "Rain", "Дождь", "Regen"},
    {65, Icon::Rain, "Heavy rain", "Сильный дождь", "Starker Regen"},
    {66, Icon::Rain, "Freezing rain", "Ледяной дождь", "Gefr. Regen"},
    {67, Icon::Rain, "Freezing rain", "Ледяной дождь", "Gefr. Regen"},
    {71, Icon::Snow, "Light snow", "Небольшой снег", "Leichter Schnee"},
    {73, Icon::Snow, "Snow", "Снег", "Schneefall"},
    {75, Icon::Snow, "Heavy snow", "Сильный снег", "Starker Schnee"},
    {77, Icon::Snow, "Snow grains", "Снежная крупа", "Schneegriesel"},
    {80, Icon::Rain, "Showers", "Ливень", "Schauer"},
    {81, Icon::Rain, "Showers", "Ливень", "Schauer"},
    {82, Icon::Rain, "Heavy showers", "Сильный ливень", "Starke Schauer"},
    {85, Icon::Snow, "Snow showers", "Снегопад", "Schneeschauer"},
    {86, Icon::Snow, "Snow showers", "Сильный снегопад", "Schneeschauer"},
    {95, Icon::Thunder, "Thunderstorm", "Гроза", "Gewitter"},
    {96, Icon::Thunder, "Thunder, hail", "Гроза с градом", "Gewitter, Hagel"},
    {99, Icon::Thunder, "Thunder, hail", "Гроза с градом", "Gewitter, Hagel"},
};

const Entry* find(int code) {
  for (const Entry& e : kTable) {
    if (e.code == code) return &e;
  }
  return nullptr;
}

const Labels kLabelsEn = {"min", "max", "wind", "precip.", "feels", "upd.", "outdated", "No data", "Unknown place", "m/s", "mm"};
const Labels kLabelsRu = {"мин", "макс", "ветер", "осадки", "ощущ.", "обн.", "устарело", "Нет данных", "Место не определено", "м/с", "мм"};
const Labels kLabelsDe = {"min", "max", "Wind", "Regen", "gefühlt", "akt.", "veraltet", "Keine Daten", "Ort unbekannt", "m/s", "mm"};

}  // namespace

Icon iconFor(int code, bool isDay) {
  const Entry* e = find(code);
  if (!e) return Icon::Unknown;
  if (!isDay) {
    if (e->icon == Icon::Clear) return Icon::ClearNight;
    if (e->icon == Icon::PartlyCloudy) return Icon::PartlyCloudyNight;
  }
  return e->icon;
}

const char* description(Lang lang, int code) {
  const Entry* e = find(code);
  if (!e) return "";
  return lang == Lang::Ru ? e->ru : lang == Lang::De ? e->de : e->en;
}

const Labels& labels(Lang lang) { return lang == Lang::Ru ? kLabelsRu : lang == Lang::De ? kLabelsDe : kLabelsEn; }

// ---- URL --------------------------------------------------------------------

int buildGeoUrl(Lang lang, char* buf, size_t size) {
  const char* l = lang == Lang::Ru ? "ru" : lang == Lang::De ? "de" : "en";
  return std::snprintf(buf, size, "https://ipwhois.app/json/?lang=%s&objects=success,city,latitude,longitude", l);
}

int buildForecastUrl(double lat, double lon, char* buf, size_t size) {
  return std::snprintf(buf, size,
                       "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                       "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
                       "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,precipitation_probability_max"
                       "&timezone=auto&forecast_days=1&wind_speed_unit=ms",
                       lat, lon);
}

// ---- Разбор ------------------------------------------------------------------

void copyUtf8(char* dst, size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  size_t n = std::strlen(src);
  if (n > dstSize - 1) {
    n = dstSize - 1;
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;  // не рвём многобайтный символ
  }
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

namespace {

float numberOrNan(JsonVariantConst v) { return v.is<float>() ? v.as<float>() : NAN; }

bool validCoord(double lat, double lon) { return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0; }

}  // namespace

bool parseGeo(const char* json, size_t len, Lang lang, uint32_t nowEpoch, Place& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  if (!(doc["success"] | false)) return false;
  JsonVariantConst la = doc["latitude"];
  JsonVariantConst lo = doc["longitude"];
  if (!la.is<double>() || !lo.is<double>()) return false;
  const double lat = la.as<double>();
  const double lon = lo.as<double>();
  if (!validCoord(lat, lon)) return false;
  // Ровно (0, 0) — типичная заглушка геобаз для «не знаю»; не принимаем за место.
  if (lat == 0.0 && lon == 0.0) return false;

  out.lat = lat;
  out.lon = lon;
  copyUtf8(out.city, sizeof(out.city), doc["city"] | "");
  out.fromIp = true;
  out.ipEpoch = nowEpoch;
  out.ipLang = static_cast<uint8_t>(lang);
  return true;
}

bool parseForecast(const char* json, size_t len, uint32_t nowEpoch, Weather& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  JsonVariantConst cur = doc["current"];
  JsonVariantConst day = doc["daily"];
  Weather w;
  w.temp = numberOrNan(cur["temperature_2m"]);
  if (std::isnan(w.temp)) return false;  // без температуры блок бессмыслен
  w.feels = numberOrNan(cur["apparent_temperature"]);
  w.windMs = numberOrNan(cur["wind_speed_10m"]);
  w.code = cur["weather_code"].is<int>() ? cur["weather_code"].as<int>() : -1;
  w.isDay = (cur["is_day"] | 1) != 0;
  w.tMin = numberOrNan(day["temperature_2m_min"][0]);
  w.tMax = numberOrNan(day["temperature_2m_max"][0]);
  w.precipMm = numberOrNan(day["precipitation_sum"][0]);
  JsonVariantConst pp = day["precipitation_probability_max"][0];
  w.precipProb = pp.is<int>() ? static_cast<int16_t>(pp.as<int>()) : -1;
  w.valid = true;
  w.fetchedEpoch = nowEpoch;
  out = w;
  return true;
}

// ---- Кэш ---------------------------------------------------------------------

std::string serializeCache(const Cache& c) {
  JsonDocument doc;
  doc["v"] = 1;
  JsonObject p = doc["place"].to<JsonObject>();
  p["lat"] = c.place.lat;
  p["lon"] = c.place.lon;
  p["city"] = c.place.city;
  p["ip"] = c.place.fromIp ? 1 : 0;
  p["ipAt"] = c.place.ipEpoch;
  p["ipLang"] = c.place.ipLang;
  if (c.weather.valid) {
    JsonObject w = doc["wx"].to<JsonObject>();
    auto put = [&w](const char* k, float v) {
      if (!std::isnan(v)) w[k] = v;
    };
    put("t", c.weather.temp);
    put("f", c.weather.feels);
    put("min", c.weather.tMin);
    put("max", c.weather.tMax);
    put("w", c.weather.windMs);
    put("p", c.weather.precipMm);
    if (c.weather.precipProb >= 0) w["pp"] = c.weather.precipProb;
    if (c.weather.code >= 0) w["c"] = c.weather.code;
    w["d"] = c.weather.isDay ? 1 : 0;
    w["at"] = c.weather.fetchedEpoch;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseCache(const char* json, size_t len, Cache& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  JsonVariantConst p = doc["place"];
  if (!p["lat"].is<double>() || !p["lon"].is<double>()) return false;
  const double lat = p["lat"].as<double>();
  const double lon = p["lon"].as<double>();
  if (!validCoord(lat, lon)) return false;

  Cache c;
  c.place.lat = lat;
  c.place.lon = lon;
  copyUtf8(c.place.city, sizeof(c.place.city), p["city"] | "");
  c.place.fromIp = (p["ip"] | 0) != 0;
  c.place.ipEpoch = p["ipAt"] | 0u;
  c.place.ipLang = static_cast<uint8_t>(p["ipLang"] | 0);

  JsonVariantConst w = doc["wx"];
  if (!w.isNull()) {
    c.weather.temp = numberOrNan(w["t"]);
    if (!std::isnan(c.weather.temp)) {
      c.weather.feels = numberOrNan(w["f"]);
      c.weather.tMin = numberOrNan(w["min"]);
      c.weather.tMax = numberOrNan(w["max"]);
      c.weather.windMs = numberOrNan(w["w"]);
      c.weather.precipMm = numberOrNan(w["p"]);
      c.weather.precipProb = w["pp"].is<int>() ? static_cast<int16_t>(w["pp"].as<int>()) : -1;
      c.weather.code = w["c"].is<int>() ? static_cast<int16_t>(w["c"].as<int>()) : -1;
      c.weather.isDay = (w["d"] | 1) != 0;
      c.weather.fetchedEpoch = w["at"] | 0u;
      c.weather.valid = true;
    }
  }
  out = c;
  return true;
}

}  // namespace weather_core
