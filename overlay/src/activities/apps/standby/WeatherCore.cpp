#include "WeatherCore.h"

#include <ArduinoJson.h>

#include <algorithm>
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

const Labels kLabelsEn = {"feels", "upd.", "outdated", "No data", "Unknown place", "m/s", "mm"};
const Labels kLabelsRu = {"ощущ.", "обн.", "устарело", "Нет данных", "Место не определено", "м/с", "мм"};
const Labels kLabelsDe = {"gefühlt", "akt.", "veraltet", "Keine Daten", "Ort unbekannt", "m/s", "mm"};

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
                       "&hourly=temperature_2m,weather_code,precipitation_probability,wind_speed_10m,is_day"
                       "&forecast_hours=24"
                       "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,"
                       "precipitation_probability_max,wind_speed_10m_max&forecast_days=7"
                       "&timezone=auto&timeformat=unixtime&wind_speed_unit=ms",
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

namespace {

bool fillWeather(const JsonDocument& doc, uint32_t nowEpoch, Weather& out) {
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

bool fillForecast(const JsonDocument& doc, uint32_t nowEpoch, Forecast& out) {
  Forecast f;
  f.utcOffsetSec = doc["utc_offset_seconds"] | 0;
  f.fetchedEpoch = nowEpoch;

  JsonArrayConst ht = doc["hourly"]["time"].as<JsonArrayConst>();
  const size_t nh = std::min<size_t>(ht.size(), kFcHours);
  for (size_t i = 0; i < nh; ++i) {
    FcHour& h = f.h[i];
    h.ts = ht[i] | 0u;
    if (h.ts == 0) return false;  // время — ключ строки; без него строка бессмысленна
    h.temp = numberOrNan(doc["hourly"]["temperature_2m"][i]);
    h.wind = numberOrNan(doc["hourly"]["wind_speed_10m"][i]);
    h.code = doc["hourly"]["weather_code"][i].is<int>() ? doc["hourly"]["weather_code"][i].as<int>() : -1;
    h.prob = doc["hourly"]["precipitation_probability"][i].is<int>()
                 ? static_cast<int8_t>(doc["hourly"]["precipitation_probability"][i].as<int>())
                 : -1;
    h.isDay = (doc["hourly"]["is_day"][i] | 1) != 0;
  }
  f.nHours = static_cast<uint8_t>(nh);

  JsonArrayConst dt = doc["daily"]["time"].as<JsonArrayConst>();
  const size_t nd = std::min<size_t>(dt.size(), kFcDays);
  for (size_t i = 0; i < nd; ++i) {
    FcDay& d = f.d[i];
    d.ts = dt[i] | 0u;
    if (d.ts == 0) return false;
    d.tMax = numberOrNan(doc["daily"]["temperature_2m_max"][i]);
    d.tMin = numberOrNan(doc["daily"]["temperature_2m_min"][i]);
    d.precipMm = numberOrNan(doc["daily"]["precipitation_sum"][i]);
    d.windMax = numberOrNan(doc["daily"]["wind_speed_10m_max"][i]);
    d.code = doc["daily"]["weather_code"][i].is<int>() ? doc["daily"]["weather_code"][i].as<int>() : -1;
    d.prob = doc["daily"]["precipitation_probability_max"][i].is<int>()
                 ? static_cast<int8_t>(doc["daily"]["precipitation_probability_max"][i].as<int>())
                 : -1;
  }
  f.nDays = static_cast<uint8_t>(nd);
  f.valid = nh > 0 || nd > 0;
  if (!f.valid) return false;
  out = f;
  return true;
}

}  // namespace

bool parseForecast(const char* json, size_t len, uint32_t nowEpoch, Weather& out, Forecast* detail) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) return false;
  if (!fillWeather(doc, nowEpoch, out)) return false;
  if (detail) fillForecast(doc, nowEpoch, *detail);  // прогноз необязателен: не вышел — остаются старые данные
  return true;
}

// ---- Кэш ---------------------------------------------------------------------

std::string serializeCache(const Cache& c) {
  JsonDocument doc;
  doc["v"] = 2;
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
  if (c.fc.valid) {
    JsonObject f = doc["fc"].to<JsonObject>();
    f["off"] = c.fc.utcOffsetSec;
    f["at"] = c.fc.fetchedEpoch;
    JsonObject h = f["h"].to<JsonObject>();
    JsonArray hts = h["ts"].to<JsonArray>(), ht = h["t"].to<JsonArray>(), hc = h["c"].to<JsonArray>(),
              hp = h["p"].to<JsonArray>(), hw = h["w"].to<JsonArray>(), hd = h["d"].to<JsonArray>();
    for (int i = 0; i < c.fc.nHours; ++i) {
      const FcHour& x = c.fc.h[i];
      hts.add(x.ts);
      if (std::isnan(x.temp)) ht.add(nullptr); else ht.add(x.temp);
      hc.add(x.code);
      hp.add(x.prob);
      if (std::isnan(x.wind)) hw.add(nullptr); else hw.add(x.wind);
      hd.add(x.isDay ? 1 : 0);
    }
    JsonObject d = f["d"].to<JsonObject>();
    JsonArray dts = d["ts"].to<JsonArray>(), dmx = d["max"].to<JsonArray>(), dmn = d["min"].to<JsonArray>(),
              dmm = d["mm"].to<JsonArray>(), dp = d["p"].to<JsonArray>(), dc = d["c"].to<JsonArray>(),
              dw = d["w"].to<JsonArray>();
    for (int i = 0; i < c.fc.nDays; ++i) {
      const FcDay& x = c.fc.d[i];
      dts.add(x.ts);
      if (std::isnan(x.tMax)) dmx.add(nullptr); else dmx.add(x.tMax);
      if (std::isnan(x.tMin)) dmn.add(nullptr); else dmn.add(x.tMin);
      if (std::isnan(x.precipMm)) dmm.add(nullptr); else dmm.add(x.precipMm);
      dp.add(x.prob);
      dc.add(x.code);
      if (std::isnan(x.windMax)) dw.add(nullptr); else dw.add(x.windMax);
    }
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
  JsonVariantConst f = doc["fc"];
  if (!f.isNull()) {
    JsonArrayConst hts = f["h"]["ts"].as<JsonArrayConst>();
    const size_t nh = std::min<size_t>(hts.size(), kFcHours);
    for (size_t i = 0; i < nh; ++i) {
      FcHour& x = c.fc.h[i];
      x.ts = hts[i] | 0u;
      x.temp = numberOrNan(f["h"]["t"][i]);
      x.code = f["h"]["c"][i].is<int>() ? f["h"]["c"][i].as<int>() : -1;
      x.prob = f["h"]["p"][i].is<int>() ? static_cast<int8_t>(f["h"]["p"][i].as<int>()) : -1;
      x.wind = numberOrNan(f["h"]["w"][i]);
      x.isDay = (f["h"]["d"][i] | 1) != 0;
    }
    JsonArrayConst dts = f["d"]["ts"].as<JsonArrayConst>();
    const size_t nd = std::min<size_t>(dts.size(), kFcDays);
    for (size_t i = 0; i < nd; ++i) {
      FcDay& x = c.fc.d[i];
      x.ts = dts[i] | 0u;
      x.tMax = numberOrNan(f["d"]["max"][i]);
      x.tMin = numberOrNan(f["d"]["min"][i]);
      x.precipMm = numberOrNan(f["d"]["mm"][i]);
      x.prob = f["d"]["p"][i].is<int>() ? static_cast<int8_t>(f["d"]["p"][i].as<int>()) : -1;
      x.code = f["d"]["c"][i].is<int>() ? f["d"]["c"][i].as<int>() : -1;
      x.windMax = numberOrNan(f["d"]["w"][i]);
    }
    c.fc.nHours = static_cast<uint8_t>(nh);
    c.fc.nDays = static_cast<uint8_t>(nd);
    c.fc.utcOffsetSec = f["off"] | 0;
    c.fc.fetchedEpoch = f["at"] | 0u;
    c.fc.valid = (nh > 0 && c.fc.h[0].ts != 0) || (nd > 0 && c.fc.d[0].ts != 0);
  }
  out = c;
  return true;
}

}  // namespace weather_core
