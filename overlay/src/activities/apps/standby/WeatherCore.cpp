#include "WeatherCore.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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

namespace {
const char* langCode(Lang lang) { return lang == Lang::Ru ? "ru" : lang == Lang::De ? "de" : "en"; }
}  // namespace

int buildGeoUrl(Lang lang, char* buf, size_t size) {
  // ip/region/country в разборе не нужны — только для журнала: видно, куда сервис «поставил» адрес (офисный прокси и т.п.).
  return std::snprintf(buf, size,
                       "https://ipwhois.app/json/?lang=%s&objects=success,ip,city,region,country,latitude,longitude",
                       langCode(lang));
}

int buildGeocodeUrl(const char* query, Lang lang, char* buf, size_t size) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const int head = std::snprintf(buf, size, "https://geocoding-api.open-meteo.com/v1/search?name=");
  if (head < 0 || static_cast<size_t>(head) >= size) return -1;
  size_t o = static_cast<size_t>(head);
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(query); *p; ++p) {
    const bool plain = *p < 0x80 && std::isalnum(*p);
    const bool unreserved = plain || *p == '-' || *p == '_' || *p == '.' || *p == '~';
    if (o + (unreserved ? 1 : 3) >= size) return -1;
    if (unreserved) {
      buf[o++] = static_cast<char>(*p);
    } else {
      buf[o++] = '%';
      buf[o++] = kHex[*p >> 4];
      buf[o++] = kHex[*p & 0x0F];
    }
  }
  const int tail = std::snprintf(buf + o, size - o, "&count=%d&language=%s&format=json", kMaxGeoHits, langCode(lang));
  if (tail < 0 || o + static_cast<size_t>(tail) >= size) return -1;
  return static_cast<int>(o + static_cast<size_t>(tail));
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

bool samePlace(double lat1, double lon1, double lat2, double lon2) {
  if (std::isnan(lat1) || std::isnan(lon1) || std::isnan(lat2) || std::isnan(lon2)) return false;
  return std::fabs(lat1 - lat2) < 0.05 && std::fabs(lon1 - lon2) < 0.05;  // 0,05° ≈ 5,5 км по широте
}

int parseGeocode(const char* json, size_t len, GeoHit* out, int maxOut) {
  // Фильтр: у мест бывают длинные списки почтовых индексов и т.п. — в документ берём только нужное.
  JsonDocument filter;
  JsonObject f = filter["results"].add<JsonObject>();
  f["name"] = true;
  f["latitude"] = true;
  f["longitude"] = true;
  f["admin1"] = true;
  f["country"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return -1;
  if (!doc.is<JsonObject>()) return -1;
  JsonArrayConst arr = doc["results"].as<JsonArrayConst>();  // нет ключа — ничего не найдено
  int n = 0;
  for (JsonVariantConst r : arr) {
    if (n >= maxOut) break;
    if (!r["latitude"].is<double>() || !r["longitude"].is<double>()) continue;
    const double lat = r["latitude"].as<double>(), lon = r["longitude"].as<double>();
    const char* name = r["name"] | "";
    if (!validCoord(lat, lon) || !name[0]) continue;
    GeoHit& h = out[n++];
    copyUtf8(h.name, sizeof(h.name), name);
    const char* a1 = r["admin1"] | "";
    const char* co = r["country"] | "";
    char region[sizeof(h.region)];
    // «Москва, Россия»; регион совпадает с названием города — всё равно пишем: так видно, что это столица региона.
    std::snprintf(region, sizeof(region), "%s%s%s", a1, (a1[0] && co[0]) ? ", " : "", co);
    copyUtf8(h.region, sizeof(h.region), region);
    h.lat = lat;
    h.lon = lon;
  }
  return n;
}

bool parseCoords(const char* text, double& lat, double& lon) {
  // Нормализуем: «;» и пробелы — разделители; запятая — десятичная, если есть другой разделитель, иначе — разделитель.
  std::string s(text);
  const bool hasSemi = s.find(';') != std::string::npos;
  const bool hasSpace = s.find_first_of(" \t") != std::string::npos;
  std::string a, b;
  auto trim = [](std::string v) {
    const size_t i = v.find_first_not_of(" \t,");
    const size_t j = v.find_last_not_of(" \t,");
    return i == std::string::npos ? std::string() : v.substr(i, j - i + 1);
  };
  if (hasSemi) {
    const size_t k = s.find(';');
    a = trim(s.substr(0, k));
    b = trim(s.substr(k + 1));
  } else if (s.find(", ") != std::string::npos) {  // «55.75, 37.62» — самая частая запись
    const size_t k = s.find(", ");
    a = trim(s.substr(0, k));
    b = trim(s.substr(k + 2));
  } else if (hasSpace) {
    const std::string t = trim(s);
    const size_t k = t.find_first_of(" \t");
    if (k == std::string::npos) return false;
    a = trim(t.substr(0, k));
    b = trim(t.substr(k + 1));
  } else if (std::count(s.begin(), s.end(), ',') == 1) {  // «55.75,37.62»
    const size_t k = s.find(',');
    a = trim(s.substr(0, k));
    b = trim(s.substr(k + 1));
  } else {
    return false;
  }
  auto num = [](std::string v, double& out) {
    if (v.empty()) return false;
    std::replace(v.begin(), v.end(), ',', '.');
    for (char ch : v) {
      if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == '+')) return false;
    }
    char* end = nullptr;
    out = std::strtod(v.c_str(), &end);
    return end && *end == '\0';
  };
  double la = 0, lo = 0;
  if (!num(a, la) || !num(b, lo) || !validCoord(la, lo)) return false;
  lat = la;
  lon = lo;
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
  if (c.place.ssid[0]) p["ssid"] = c.place.ssid;
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
    if (!std::isnan(c.weather.atLat) && !std::isnan(c.weather.atLon)) {
      w["la"] = c.weather.atLat;
      w["lo"] = c.weather.atLon;
    }
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
  copyUtf8(c.place.ssid, sizeof(c.place.ssid), p["ssid"] | "");

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
      // Кэш до появления «la/lo» писал погоду только для того места, что лежит рядом, — к нему и относим.
      c.weather.atLat = w["la"].is<double>() ? w["la"].as<double>() : lat;
      c.weather.atLon = w["lo"].is<double>() ? w["lo"].as<double>() : lon;
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

// ---- Настройки -----------------------------------------------------------------

std::string serializeSettings(const Settings& s) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["auto"] = s.autoLocation;
  doc["log"] = s.sdLog;
  JsonObject m = doc["manual"].to<JsonObject>();
  m["city"] = s.manual.city;
  m["lat"] = s.manual.lat;
  m["lon"] = s.manual.lon;
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseSettings(const char* json, size_t len, Settings& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) || !doc.is<JsonObject>()) return false;
  Settings s;  // чего нет в файле — значение по умолчанию из CalendarConfig.h
  if (doc["auto"].is<bool>()) s.autoLocation = doc["auto"].as<bool>();
  if (doc["log"].is<bool>()) s.sdLog = doc["log"].as<bool>();
  JsonVariantConst m = doc["manual"];
  if (m["lat"].is<double>() && m["lon"].is<double>()) {
    const double lat = m["lat"].as<double>(), lon = m["lon"].as<double>();
    if (!validCoord(lat, lon)) return false;
    s.manual.lat = lat;
    s.manual.lon = lon;
    copyUtf8(s.manual.city, sizeof(s.manual.city), m["city"] | "");
  }
  s.manual.fromIp = false;
  out = s;
  return true;
}

}  // namespace weather_core
