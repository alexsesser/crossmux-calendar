#include "WeatherCore.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "SunTimes.h"

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
    {68, Icon::Snow, "Sleet", "Мокрый снег", "Schneeregen"},  // 68/69, 83/84 — только от MET Norway
    {69, Icon::Snow, "Heavy sleet", "Сильный мокрый снег", "Starker Schneeregen"},
    {77, Icon::Snow, "Snow grains", "Снежная крупа", "Schneegriesel"},
    {80, Icon::Rain, "Showers", "Ливень", "Schauer"},
    {81, Icon::Rain, "Showers", "Ливень", "Schauer"},
    {82, Icon::Rain, "Heavy showers", "Сильный ливень", "Starke Schauer"},
    {83, Icon::Snow, "Sleet showers", "Мокрый снег", "Schneeregenschauer"},
    {84, Icon::Snow, "Sleet showers", "Сильный мокрый снег", "Schneeregenschauer"},
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

const char* providerName(Provider p) { return p == Provider::MetNo ? "MET Norway" : "Open-Meteo.com"; }

const char* routeName(Route r) {
  switch (r) {
    case Route::OpenMeteoHttps:
      return "Open-Meteo";
    case Route::OpenMeteoHttp:
      return "Open-Meteo по HTTP";
    case Route::MetNo:
      return "MET Norway";
    case Route::OpenMeteoMirror:
      return "Open-Meteo, другой адрес";
  }
  return "?";
}

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

namespace {

// query в UTF-8 → %XX (кроме букв/цифр ASCII и «-_.~») в buf начиная с o. Новая длина или -1, если не влезло.
int appendEncoded(const char* query, char* buf, size_t size, size_t o) {
  static constexpr char kHex[] = "0123456789ABCDEF";
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
  buf[o] = '\0';
  return static_cast<int>(o);
}

int searchUrl(const char* head, const char* query, const char* tailFmt, Lang lang, char* buf, size_t size) {
  const int h = std::snprintf(buf, size, "%s", head);
  if (h < 0 || static_cast<size_t>(h) >= size) return -1;
  const int o = appendEncoded(query, buf, size, static_cast<size_t>(h));
  if (o < 0) return -1;
  const int tail = std::snprintf(buf + o, size - static_cast<size_t>(o), tailFmt, kMaxGeoHits, langCode(lang));
  if (tail < 0 || static_cast<size_t>(o) + static_cast<size_t>(tail) >= size) return -1;
  return o + tail;
}

}  // namespace

int buildGeocodeUrl(const char* query, Lang lang, char* buf, size_t size, bool https) {
  return searchUrl(https ? "https://geocoding-api.open-meteo.com/v1/search?name="
                         : "http://geocoding-api.open-meteo.com/v1/search?name=",
                   query, "&count=%d&language=%s&format=json", lang, buf, size);
}

int buildNominatimUrl(const char* query, Lang lang, char* buf, size_t size) {
  // featureType=settlement — только города, посёлки, деревни (без областей и улиц).
  return searchUrl("https://nominatim.openstreetmap.org/search?q=", query,
                   "&format=jsonv2&limit=%d&featureType=settlement&accept-language=%s", lang, buf, size);
}

int buildForecastUrl(double lat, double lon, char* buf, size_t size, bool https, const char* host) {
  char where[200];
  if (!host || !host[0]) host = "api.open-meteo.com";
  if (https) {
    std::snprintf(where, sizeof(where), "https://%s/v1/forecast?latitude=%.4f&longitude=%.4f", host, lat, lon);
  } else {
    std::snprintf(where, sizeof(where), "http://%s/v1/forecast?latitude=%.2f&longitude=%.2f", host, lat, lon);
  }
  return std::snprintf(buf, size,
                       "%s"
                       "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
                       "&hourly=temperature_2m,weather_code,precipitation_probability,wind_speed_10m,is_day"
                       "&forecast_hours=24"
                       "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,"
                       "precipitation_probability_max,wind_speed_10m_max&forecast_days=7"
                       "&timezone=auto&timeformat=unixtime&wind_speed_unit=ms",
                       where);
}

int buildMetNoUrl(double lat, double lon, char* buf, size_t size) {
  // Не больше 4 знаков после запятой — так просит MET (иначе их кэш не работает и запрос могут отклонить).
  return std::snprintf(buf, size, "https://api.met.no/weatherapi/locationforecast/2.0/complete?lat=%.4f&lon=%.4f", lat, lon);
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

// ---- MET Norway ----------------------------------------------------------------

int metSymbolToWmo(const char* symbol) {
  if (!symbol || !symbol[0]) return -1;
  std::string s(symbol);
  const size_t us = s.find('_');  // «_day», «_night», «_polartwilight» — день/ночь считаем сами
  if (us != std::string::npos) s.resize(us);
  auto has = [&s](const char* w) { return s.find(w) != std::string::npos; };
  if (s == "clearsky") return 0;
  if (s == "fair") return 1;
  if (s == "partlycloudy") return 2;
  if (s == "cloudy") return 3;
  if (s == "fog") return 45;
  if (has("thunder")) return 95;
  // «light…» (у MET встречается и «lights…» — опечатка в их API), обычный, «heavy…».
  const int level = s.compare(0, 5, "light") == 0 ? 0 : s.compare(0, 5, "heavy") == 0 ? 2 : 1;
  const bool showers = has("showers");
  if (has("sleet")) return showers ? (level == 0 ? 83 : 84) : (level == 0 ? 68 : 69);
  if (has("snow")) return showers ? (level == 2 ? 86 : 85) : (level == 0 ? 71 : level == 1 ? 73 : 75);
  if (has("rain")) return showers ? 80 + level : 61 + 2 * level;
  return -1;
}

namespace {

// «2026-09-26T13:00:00Z» → Unix; 0 — не разобрать.
uint32_t isoUtcToEpoch(const char* s) {
  int y = 0;
  unsigned mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
  if (!s || std::sscanf(s, "%d-%u-%uT%u:%u:%u", &y, &mo, &d, &hh, &mm, &ss) != 6) return 0;
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60) return 0;
  return static_cast<uint32_t>(calendar_core::daysFromCivil(y, mo, d)) * 86400u + hh * 3600u + mm * 60u + ss;
}

int32_t localDay(uint32_t epoch, int32_t off) {
  const int64_t t = static_cast<int64_t>(epoch) + off;
  return static_cast<int32_t>(t >= 0 ? t / 86400 : (t - 86399) / 86400);
}

// День или ночь в момент t — по восходу и закату места (у MET признака «день» нет, а значки без суффикса бывают).
class DayNight {
 public:
  DayNight(double lat, double lon, int32_t off) : lat_(lat), lon_(lon), off_(off) {}
  bool isDay(uint32_t t) {
    const int32_t day = localDay(t, off_);
    if (day != day_) {
      int y;
      unsigned m, d;
      calendar_core::civilFromDays(day, y, m, d);
      sun_ = sun_times::compute(y, m, d, lat_, lon_, off_ / 60);
      day_ = day;
    }
    if (!sun_.valid || sun_.polarDay) return true;
    if (sun_.polarNight) return false;
    const int minute = static_cast<int>((static_cast<int64_t>(t) + off_ - static_cast<int64_t>(day) * 86400) / 60);
    if (sun_.sunriseMin <= sun_.sunsetMin) return minute >= sun_.sunriseMin && minute < sun_.sunsetMin;
    return minute >= sun_.sunriseMin || minute < sun_.sunsetMin;  // закат «после полуночи» по местным часам
  }

 private:
  double lat_, lon_;
  int32_t off_;
  int32_t day_ = INT32_MIN;
  sun_times::Result sun_{};
};

// «Ощущается» по Стедману (так же считает Open-Meteo), если сервис его не дал: температура, влажность, ветер.
float apparent(float t, float rh, float wind) {
  if (std::isnan(t) || std::isnan(rh) || std::isnan(wind)) return NAN;
  const float e = rh / 100.0f * 6.105f * std::exp(17.27f * t / (237.7f + t));
  return t + 0.33f * e - 0.70f * wind - 4.00f;
}

void minTo(float& a, float v) {
  if (!std::isnan(v) && (std::isnan(a) || v < a)) a = v;
}
void maxTo(float& a, float v) {
  if (!std::isnan(v) && (std::isnan(a) || v > a)) a = v;
}
void addTo(float& a, float v) {
  if (!std::isnan(v)) a = std::isnan(a) ? v : a + v;
}

}  // namespace

bool parseMetNo(const char* json, size_t len, uint32_t nowEpoch, double lat, double lon, int32_t off, Weather& out,
                Forecast* detail) {
  // Фильтр: из ≈ 65 КБ ответа в документ идут только нужные поля (давление, облачность и т.п. — мимо).
  JsonDocument filter;
  JsonObject fe = filter["properties"]["timeseries"].add<JsonObject>();
  fe["time"] = true;
  JsonObject fi = fe["data"]["instant"]["details"].to<JsonObject>();
  for (const char* k : {"air_temperature", "apparent_air_temperature", "wind_speed", "relative_humidity"}) fi[k] = true;
  for (const char* period : {"next_1_hours", "next_6_hours"}) {
    fe["data"][period]["summary"]["symbol_code"] = true;
    JsonObject fd = fe["data"][period]["details"].to<JsonObject>();
    for (const char* k : {"precipitation_amount", "probability_of_precipitation", "air_temperature_max", "air_temperature_min"}) {
      fd[k] = true;
    }
  }
  JsonDocument doc;
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return false;
  JsonArrayConst series = doc["properties"]["timeseries"].as<JsonArrayConst>();
  if (series.size() == 0) return false;

  DayNight dn(lat, lon, off);
  const int32_t today = localDay(nowEpoch, off);
  const uint32_t hourStart = nowEpoch - nowEpoch % 3600u;
  struct DayAgg {
    float tMin = NAN, tMax = NAN, mm = NAN, wind = NAN;
    int code = -1, prob = -1;
    bool any = false;
  } days[kFcDays];
  Forecast f;
  f.utcOffsetSec = off;
  f.fetchedEpoch = nowEpoch;
  f.provider = Provider::MetNo;

  // «Сейчас» — последний шаг ряда, начавшийся не позже текущего момента (первый шаг — текущий час).
  JsonVariantConst cur;
  for (JsonVariantConst e : series) {
    const uint32_t t = isoUtcToEpoch(e["time"] | "");
    if (t == 0) continue;
    if (t <= nowEpoch || cur.isNull()) cur = e;

    JsonVariantConst inst = e["data"]["instant"]["details"];
    JsonVariantConst n1 = e["data"]["next_1_hours"];
    JsonVariantConst n6 = e["data"]["next_6_hours"];
    const float temp = numberOrNan(inst["air_temperature"]);
    const float wind = numberOrNan(inst["wind_speed"]);

    // Часы: только часовые шаги, начиная с текущего часа.
    if (!n1.isNull() && t >= hourStart && f.nHours < kFcHours) {
      FcHour& h = f.h[f.nHours++];
      h.ts = t;
      h.temp = temp;
      h.wind = wind;
      h.code = static_cast<int16_t>(metSymbolToWmo(n1["summary"]["symbol_code"] | ""));
      h.prob = n1["details"]["probability_of_precipitation"].is<float>()
                   ? static_cast<int8_t>(std::lround(n1["details"]["probability_of_precipitation"].as<float>()))
                   : -1;
      h.mm = numberOrNan(n1["details"]["precipitation_amount"]);
      h.isDay = dn.isDay(t);
    }

    // Дни: осадки и «самая суровая» погода — по часовым шагам, где они есть, иначе по шестичасовым (они не
    // перекрываются: часовые идут до начала первого шестичасового). Температура — мгновенная в каждом шаге плюс
    // мин/макс шестичасовых отрезков.
    const int32_t di = localDay(t, off) - today;
    if (di < 0 || di >= kFcDays) continue;
    DayAgg& a = days[di];
    a.any = true;
    minTo(a.tMin, temp);
    maxTo(a.tMax, temp);
    maxTo(a.wind, wind);
    JsonVariantConst per = !n1.isNull() ? n1 : n6;
    if (per.isNull()) continue;
    addTo(a.mm, numberOrNan(per["details"]["precipitation_amount"]));
    a.code = std::max(a.code, metSymbolToWmo(per["summary"]["symbol_code"] | ""));
    if (per["details"]["probability_of_precipitation"].is<float>()) {
      a.prob = std::max(a.prob, static_cast<int>(std::lround(per["details"]["probability_of_precipitation"].as<float>())));
    }
    if (n1.isNull()) {
      minTo(a.tMin, numberOrNan(n6["details"]["air_temperature_min"]));
      maxTo(a.tMax, numberOrNan(n6["details"]["air_temperature_max"]));
    }
  }
  if (cur.isNull()) return false;

  JsonVariantConst ci = cur["data"]["instant"]["details"];
  Weather w;
  w.temp = numberOrNan(ci["air_temperature"]);
  if (std::isnan(w.temp)) return false;
  w.windMs = numberOrNan(ci["wind_speed"]);
  w.feels = numberOrNan(ci["apparent_air_temperature"]);
  if (std::isnan(w.feels)) w.feels = apparent(w.temp, numberOrNan(ci["relative_humidity"]), w.windMs);
  const char* sym = cur["data"]["next_1_hours"]["summary"]["symbol_code"] | "";
  if (!sym[0]) sym = cur["data"]["next_6_hours"]["summary"]["symbol_code"] | "";
  w.code = static_cast<int16_t>(metSymbolToWmo(sym));
  w.isDay = dn.isDay(nowEpoch);
  if (days[0].any) {
    w.tMin = days[0].tMin;
    w.tMax = days[0].tMax;
    w.precipMm = days[0].mm;
    w.precipProb = static_cast<int16_t>(days[0].prob);
  }
  w.valid = true;
  w.fetchedEpoch = nowEpoch;
  w.provider = Provider::MetNo;

  for (int i = 0; i < kFcDays; ++i) {
    if (!days[i].any) break;
    FcDay& d = f.d[f.nDays++];
    d.ts = static_cast<uint32_t>(static_cast<int64_t>(today + i) * 86400 - off);
    d.tMin = days[i].tMin;
    d.tMax = days[i].tMax;
    d.precipMm = days[i].mm;
    d.windMax = days[i].wind;
    d.code = static_cast<int16_t>(days[i].code);
    d.prob = static_cast<int8_t>(days[i].prob);
  }
  f.valid = f.nHours > 0 || f.nDays > 0;
  out = w;
  if (detail && f.valid) *detail = f;
  return true;
}

int parseNominatim(const char* json, size_t len, GeoHit* out, int maxOut) {
  JsonDocument filter;
  JsonObject f = filter.add<JsonObject>();
  f["name"] = true;
  f["lat"] = true;
  f["lon"] = true;
  f["display_name"] = true;
  f["addresstype"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return -1;
  if (!doc.is<JsonArray>()) return -1;
  int n = 0;
  bool town[kMaxGeoHits] = {};  // запись — сам населённый пункт (а не, например, одноимённый регион)
  for (JsonVariantConst r : doc.as<JsonArrayConst>()) {
    // Координаты у Nominatim — строками.
    const char* la = r["lat"] | "";
    const char* lo = r["lon"] | "";
    char* e1 = nullptr;
    char* e2 = nullptr;
    const double lat = std::strtod(la, &e1), lon = std::strtod(lo, &e2);
    const char* name = r["name"] | "";
    if (e1 == la || e2 == lo || !validCoord(lat, lon) || !name[0]) continue;
    // «Москва, Центральный федеральный округ, Россия» → регион — всё после названия.
    const char* dn = r["display_name"] | "";
    const size_t nl = std::strlen(name);
    const char* region = (std::strncmp(dn, name, nl) == 0 && dn[nl] == ',') ? dn + nl + 1 : dn;
    while (*region == ' ') ++region;
    const char* type = r["addresstype"] | "";
    const bool isTown = !std::strcmp(type, "city") || !std::strcmp(type, "town") || !std::strcmp(type, "village") ||
                        !std::strcmp(type, "hamlet");
    GeoHit h;
    copyUtf8(h.name, sizeof(h.name), name);
    copyUtf8(h.region, sizeof(h.region), region);
    h.lat = lat;
    h.lon = lon;
    // Одинаковые строки в списке (город-регион «Москва» и город «Москва») — одна, с координатами самого города.
    int same = -1;
    for (int i = 0; i < n && i < maxOut; ++i) {
      if (!std::strcmp(out[i].name, h.name) && !std::strcmp(out[i].region, h.region)) same = i;
    }
    if (same >= 0) {
      if (isTown && !town[same]) {
        out[same] = h;
        town[same] = true;
      }
      continue;
    }
    if (n >= maxOut || n >= kMaxGeoHits) continue;
    out[n] = h;
    town[n] = isTown;
    ++n;
  }
  return n;
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
  if (c.route != Route::OpenMeteoHttps) {
    doc["route"] = static_cast<int>(c.route);
    doc["routeAt"] = c.routeAt;
  }
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
    if (c.weather.provider != Provider::OpenMeteo) w["src"] = static_cast<int>(c.weather.provider);
    if (!std::isnan(c.weather.atLat) && !std::isnan(c.weather.atLon)) {
      w["la"] = c.weather.atLat;
      w["lo"] = c.weather.atLon;
    }
  }
  if (c.fc.valid) {
    JsonObject f = doc["fc"].to<JsonObject>();
    f["off"] = c.fc.utcOffsetSec;
    f["at"] = c.fc.fetchedEpoch;
    if (c.fc.provider != Provider::OpenMeteo) f["src"] = static_cast<int>(c.fc.provider);
    JsonObject h = f["h"].to<JsonObject>();
    JsonArray hts = h["ts"].to<JsonArray>(), ht = h["t"].to<JsonArray>(), hc = h["c"].to<JsonArray>(),
              hp = h["p"].to<JsonArray>(), hw = h["w"].to<JsonArray>(), hd = h["d"].to<JsonArray>();
    bool anyMm = false;
    for (int i = 0; i < c.fc.nHours; ++i) anyMm = anyMm || !std::isnan(c.fc.h[i].mm);
    JsonArray hm;
    if (anyMm) hm = h["m"].to<JsonArray>();
    for (int i = 0; i < c.fc.nHours; ++i) {
      const FcHour& x = c.fc.h[i];
      hts.add(x.ts);
      if (std::isnan(x.temp)) ht.add(nullptr); else ht.add(x.temp);
      hc.add(x.code);
      hp.add(x.prob);
      if (std::isnan(x.wind)) hw.add(nullptr); else hw.add(x.wind);
      hd.add(x.isDay ? 1 : 0);
      if (anyMm) {
        if (std::isnan(x.mm)) hm.add(nullptr); else hm.add(x.mm);
      }
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
  const int route = doc["route"] | 0;
  if (route > 0 && route < kRoutes) {
    c.route = static_cast<Route>(route);
    c.routeAt = doc["routeAt"] | 0u;
  }

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
      c.weather.provider = (w["src"] | 0) == 1 ? Provider::MetNo : Provider::OpenMeteo;
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
      x.mm = numberOrNan(f["h"]["m"][i]);
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
    c.fc.provider = (f["src"] | 0) == 1 ? Provider::MetNo : Provider::OpenMeteo;
    c.fc.valid = (nh > 0 && c.fc.h[0].ts != 0) || (nd > 0 && c.fc.d[0].ts != 0);
  }
  out = c;
  return true;
}

// ---- Настройки -----------------------------------------------------------------

void addSaved(Settings& s, const Place& p) {
  Place keep[kMaxSavedPlaces];
  int n = 0;
  keep[n] = p;
  keep[n].fromIp = false;
  keep[n].ipEpoch = 0;
  keep[n].ssid[0] = '\0';
  ++n;
  for (int i = 0; i < s.nSaved && n < kMaxSavedPlaces; ++i) {
    if (!samePlace(s.saved[i].lat, s.saved[i].lon, p.lat, p.lon)) keep[n++] = s.saved[i];
  }
  for (int i = 0; i < n; ++i) s.saved[i] = keep[i];
  s.nSaved = n;
}

void removeSaved(Settings& s, int i) {
  if (i < 0 || i >= s.nSaved) return;
  for (int k = i; k + 1 < s.nSaved; ++k) s.saved[k] = s.saved[k + 1];
  --s.nSaved;
}

std::string serializeSettings(const Settings& s) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["auto"] = s.autoLocation;
  doc["log"] = s.sdLog;
  JsonObject m = doc["manual"].to<JsonObject>();
  m["city"] = s.manual.city;
  m["lat"] = s.manual.lat;
  m["lon"] = s.manual.lon;
  if (s.nSaved > 0) {
    JsonArray a = doc["saved"].to<JsonArray>();
    for (int i = 0; i < s.nSaved; ++i) {
      JsonObject o = a.add<JsonObject>();
      o["city"] = s.saved[i].city;
      o["lat"] = s.saved[i].lat;
      o["lon"] = s.saved[i].lon;
    }
  }
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
  for (JsonVariantConst o : doc["saved"].as<JsonArrayConst>()) {
    if (s.nSaved >= kMaxSavedPlaces) break;
    if (!o["lat"].is<double>() || !o["lon"].is<double>()) continue;
    const double la = o["lat"].as<double>(), lo = o["lon"].as<double>();
    if (!validCoord(la, lo)) continue;
    Place& p = s.saved[s.nSaved++];
    p.lat = la;
    p.lon = lo;
    copyUtf8(p.city, sizeof(p.city), o["city"] | "");
  }
  out = s;
  return true;
}

// ---- История -------------------------------------------------------------------

const HistDay* HistStore::find(int32_t date, double lat, double lon) const {
  const HistDay* best = nullptr;
  for (int i = 0; i < n; ++i) {
    const HistDay& h = d[i];
    if (h.date != date || !samePlace(h.lat, h.lon, lat, lon)) continue;
    if (!best || (h.src == HistSource::Archive && best->src != HistSource::Archive)) best = &h;
  }
  return best;
}

void HistStore::put(const HistDay& h) {
  int slot = -1;
  for (int i = 0; i < n; ++i) {
    if (d[i].date == h.date && samePlace(d[i].lat, d[i].lon, h.lat, h.lon)) {
      if (d[i].src == HistSource::Archive && h.src == HistSource::Recorded) return;  // архив точнее
      slot = i;
      break;
    }
  }
  if (slot < 0 && n < kHistMax) slot = n++;
  if (slot < 0) {  // полон — вытесняем самое давно записанное
    slot = 0;
    for (int i = 1; i < n; ++i) {
      if (d[i].savedAt < d[slot].savedAt) slot = i;
    }
  }
  d[slot] = h;
}

std::string serializeHistory(const HistStore& hs) {
  JsonDocument doc;
  doc["v"] = 1;
  JsonArray a = doc["d"].to<JsonArray>();
  for (int i = 0; i < hs.n; ++i) {
    const HistDay& h = hs.d[i];
    JsonObject o = a.add<JsonObject>();
    o["dt"] = h.date;
    o["la"] = h.lat;
    o["lo"] = h.lon;
    auto put = [&o](const char* k, float v) {
      if (!std::isnan(v)) o[k] = v;
    };
    put("mx", h.tMax);
    put("mn", h.tMin);
    put("mm", h.mm);
    put("w", h.wind);
    if (h.code >= 0) o["c"] = h.code;
    o["s"] = static_cast<int>(h.src);
    o["at"] = h.savedAt;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseHistory(const char* json, size_t len, HistStore& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) || !doc.is<JsonObject>()) return false;
  HistStore hs;
  for (JsonVariantConst o : doc["d"].as<JsonArrayConst>()) {
    if (hs.n >= kHistMax) break;
    HistDay h;
    h.date = o["dt"] | 0;
    const int src = o["s"] | 0;
    if (h.date <= 0 || src < 1 || src > 2) continue;
    h.lat = numberOrNan(o["la"]);
    h.lon = numberOrNan(o["lo"]);
    h.tMax = numberOrNan(o["mx"]);
    h.tMin = numberOrNan(o["mn"]);
    h.mm = numberOrNan(o["mm"]);
    h.wind = numberOrNan(o["w"]);
    h.code = o["c"].is<int>() ? static_cast<int16_t>(o["c"].as<int>()) : -1;
    h.src = static_cast<HistSource>(src);
    h.savedAt = o["at"] | 0u;
    hs.d[hs.n++] = h;
  }
  out = hs;
  return true;
}

int buildArchiveUrl(double lat, double lon, int32_t from, int32_t to, char* buf, size_t size, bool https) {
  return std::snprintf(buf, size,
                       "%s://archive-api.open-meteo.com/v1/archive?latitude=%.2f&longitude=%.2f"
                       "&start_date=%04d-%02d-%02d&end_date=%04d-%02d-%02d"
                       "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,wind_speed_10m_max"
                       "&timezone=auto&wind_speed_unit=ms",
                       https ? "https" : "http", lat, lon, static_cast<int>(from / 10000), static_cast<int>(from / 100 % 100),
                       static_cast<int>(from % 100), static_cast<int>(to / 10000), static_cast<int>(to / 100 % 100),
                       static_cast<int>(to % 100));
}

int parseArchive(const char* json, size_t len, double lat, double lon, uint32_t nowEpoch, HistDay* out, int maxOut) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) || !doc.is<JsonObject>()) return -1;
  JsonVariantConst day = doc["daily"];
  JsonArrayConst time = day["time"].as<JsonArrayConst>();
  if (time.isNull()) return -1;
  int n = 0;
  for (size_t i = 0; i < time.size() && n < maxOut; ++i) {
    // Даты — строками «ГГГГ-ММ-ДД» (без timeformat=unixtime: так дата уже местная для места).
    int y = 0;
    unsigned m = 0, dd = 0;
    if (std::sscanf(time[i] | "", "%d-%u-%u", &y, &m, &dd) != 3) continue;
    HistDay h;
    h.date = y * 10000 + static_cast<int32_t>(m) * 100 + static_cast<int32_t>(dd);
    h.lat = static_cast<float>(lat);
    h.lon = static_cast<float>(lon);
    h.tMax = numberOrNan(day["temperature_2m_max"][i]);
    h.tMin = numberOrNan(day["temperature_2m_min"][i]);
    if (std::isnan(h.tMax) && std::isnan(h.tMin)) continue;  // архив ещё не дошёл до этого дня
    h.mm = numberOrNan(day["precipitation_sum"][i]);
    h.wind = numberOrNan(day["wind_speed_10m_max"][i]);
    h.code = day["weather_code"][i].is<int>() ? static_cast<int16_t>(day["weather_code"][i].as<int>()) : -1;
    h.src = HistSource::Archive;
    h.savedAt = nowEpoch;
    out[n++] = h;
  }
  return n;
}

}  // namespace weather_core
