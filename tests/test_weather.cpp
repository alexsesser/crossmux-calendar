// Тесты WeatherCore: настоящие ответы API (tests/data) + негативные случаи. Печатает FAIL-строки; код возврата = число ошибок.
// Координаты после JSON сравниваем с допуском 1e-4° (≈ 10 м): ArduinoJson 7 хранит короткие числа как float.
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <fstream>
#include <sstream>
#include <string>

#include "WeatherCore.h"
using namespace weather_core;
using calendar_core::Lang;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static std::string slurp(const char* p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "data";
  // --- Open-Meteo: реальный ответ ---
  {
    const std::string j = slurp((dir + "/openmeteo_moscow_7d.json").c_str());
    Weather w; CHECK(parseForecast(j.data(), j.size(), 1000, w));
    CHECK(w.valid && !std::isnan(w.temp) && w.temp > -60 && w.temp < 60);
    CHECK(!std::isnan(w.feels) && !std::isnan(w.tMin) && !std::isnan(w.tMax) && w.tMin <= w.tMax);
    CHECK(!std::isnan(w.windMs) && w.windMs >= 0 && !std::isnan(w.precipMm));
    CHECK(w.code >= 0 && iconFor(w.code, w.isDay) != Icon::Unknown);
    CHECK(w.fetchedEpoch == 1000);
  }
  // --- ipwhois: реальный ответ ---
  {
    const std::string j = slurp((dir + "/ipwhois_ru.json").c_str());
    Place p; CHECK(parseGeo(j.data(), j.size(), Lang::Ru, 555, p));
    CHECK(p.fromIp && p.ipEpoch == 555 && p.ipLang == 1 && std::string(p.city).size() > 0);
    CHECK(p.lat != kDefaultLat);
  }
  // --- Синтетика: отсутствующие поля → NaN, а не «0» ---
  {
    const char* j = R"({"current":{"temperature_2m":-3.4,"weather_code":71},"daily":{"temperature_2m_max":[null],"precipitation_probability_max":[null]}})";
    Weather w; CHECK(parseForecast(j, std::strlen(j), 1, w));
    CHECK(w.valid && w.temp < -3.3f && w.temp > -3.5f);
    CHECK(std::isnan(w.feels) && std::isnan(w.tMax) && std::isnan(w.tMin) && std::isnan(w.windMs) && std::isnan(w.precipMm));
    CHECK(w.precipProb == -1 && w.code == 71);
  }
  // --- Негативные: мусор/ошибки не должны портить прежние данные ---
  {
    Weather keep; keep.valid = true; keep.temp = 7.f; keep.fetchedEpoch = 42;
    for (const char* bad : {"", "not json", "{}", R"({"error":true,"reason":"x"})", R"({"current":{"temperature_2m":null}})", "{\"current\":{\"temperature_2m\":"}) {
      Weather w = keep; CHECK(!parseForecast(bad, std::strlen(bad), 1, w)); CHECK(w.temp == 7.f && w.fetchedEpoch == 42);
    }
    Place p; p.lat = 1; p.lon = 2;
    for (const char* bad : {"", "x", R"({"success":false,"message":"limit"})", R"({"success":true,"latitude":0,"longitude":0,"city":"?"})",
                            R"({"success":true,"latitude":123,"longitude":5})", R"({"success":true,"city":"A"})"}) {
      Place q = p; CHECK(!parseGeo(bad, std::strlen(bad), Lang::En, 1, q)); CHECK(q.lat == 1 && q.lon == 2 && !q.fromIp);
    }
    // Пустое название города — координаты принимаем, город пустой
    const char* nocity = R"({"success":true,"latitude":10.5,"longitude":20.5})";
    Place q; CHECK(parseGeo(nocity, std::strlen(nocity), Lang::En, 1, q)); CHECK(q.city[0] == '\0' && q.lat == 10.5);
  }
  // --- Кэш: круг туда-обратно, включая NaN и кириллицу ---
  {
    Cache c; c.place.lat = 50.1; c.place.lon = 8.68; copyUtf8(c.place.city, sizeof(c.place.city), "Франкфурт-на-Майне");
    c.place.fromIp = true; c.place.ipEpoch = 9; c.place.ipLang = 1;
    c.weather.valid = true; c.weather.temp = 18.5f; c.weather.tMax = 20; c.weather.code = 3; c.weather.isDay = false; c.weather.fetchedEpoch = 77;
    const std::string s = serializeCache(c);
    Cache r; CHECK(parseCache(s.data(), s.size(), r));
    CHECK(std::string(r.place.city) == "Франкфурт-на-Майне" && r.place.fromIp && r.place.ipEpoch == 9 && r.place.ipLang == 1);
    CHECK(r.weather.valid && r.weather.temp == 18.5f && r.weather.tMax == 20 && std::isnan(r.weather.feels) && std::isnan(r.weather.tMin));
    CHECK(r.weather.code == 3 && !r.weather.isDay && r.weather.fetchedEpoch == 77 && r.weather.precipProb == -1);
    // Кэш без погоды (только место) — место сохраняется
    c.weather = Weather{}; const std::string s2 = serializeCache(c);
    Cache r2; CHECK(parseCache(s2.data(), s2.size(), r2)); CHECK(!r2.weather.valid && r2.place.fromIp);
    // Битый/пустой файл — false и out не тронут
    Cache keep; keep.place.lat = 11; for (const char* bad : {"", "{", "[]", R"({"place":{"lat":"x"}})", R"({"place":{"lat":99,"lon":0}})"}) {
      Cache t = keep; CHECK(!parseCache(bad, std::strlen(bad), t)); CHECK(t.place.lat == 11);
    }
  }
  // --- Прогноз 24 ч + 7 дней: настоящий ответ ---
  {
    const std::string j = slurp((dir + "/openmeteo_moscow_7d.json").c_str());
    Forecast f; Weather w0; CHECK(parseForecast(j.data(), j.size(), 5000, w0, &f)); CHECK(w0.valid);
    CHECK(f.valid && f.nHours == 24 && f.nDays == 7 && f.utcOffsetSec == 10800 && f.fetchedEpoch == 5000);
    for (int i = 1; i < 24; ++i) CHECK(f.h[i].ts == f.h[i - 1].ts + 3600);
    for (int i = 1; i < 7; ++i) CHECK(f.d[i].ts == f.d[i - 1].ts + 86400);
    for (int i = 0; i < 7; ++i) CHECK(!std::isnan(f.d[i].tMax) && !std::isnan(f.d[i].tMin) && f.d[i].tMin <= f.d[i].tMax && f.d[i].code >= 0 && f.d[i].prob >= 0);
    for (int i = 0; i < 24; ++i) CHECK(!std::isnan(f.h[i].temp) && f.h[i].code >= 0 && f.h[i].prob >= 0);
    // День/ночь по часам: в ответе есть и то и другое
    bool day = false, night = false; for (int i = 0; i < 24; ++i) (f.h[i].isDay ? day : night) = true; CHECK(day && night);
    // кэш v2: круг с NaN
    Cache c; c.fc = f; c.fc.h[3].temp = NAN; c.fc.d[2].windMax = NAN; c.fc.d[1].prob = -1;
    const std::string sj = serializeCache(c); Cache r; CHECK(parseCache(sj.data(), sj.size(), r));
    CHECK(r.fc.valid && r.fc.nHours == 24 && r.fc.nDays == 7 && r.fc.utcOffsetSec == 10800 && r.fc.fetchedEpoch == 5000);
    CHECK(std::isnan(r.fc.h[3].temp) && r.fc.h[4].temp == f.h[4].temp && r.fc.h[23].ts == f.h[23].ts && r.fc.h[5].isDay == f.h[5].isDay);
    CHECK(std::isnan(r.fc.d[2].windMax) && r.fc.d[1].prob == -1 && r.fc.d[6].code == f.d[6].code && r.fc.d[0].ts == f.d[0].ts);
    // кэш v1 (без прогноза) читается: прогноза нет, место есть
    const char* v1 = R"({"v":1,"place":{"lat":10.5,"lon":20.5,"city":"X","ip":1,"ipAt":3,"ipLang":0}})";
    Cache o; CHECK(parseCache(v1, std::strlen(v1), o)); CHECK(!o.fc.valid && !o.weather.valid && o.place.lat == 10.5);
    // Мусор в прогнозе при валидной текущей погоде: погода разобрана (true), а прогноз не тронут
    for (const char* bad : {"{\"current\":{\"temperature_2m\":5.0},\"hourly\":{\"time\":[0]}}", "{\"current\":{\"temperature_2m\":5.0},\"daily\":{\"time\":[]},\"hourly\":{\"time\":[]}}", "{\"current\":{\"temperature_2m\":5.0}}"}) {
      Forecast g = f; Weather wb; CHECK(parseForecast(bad, std::strlen(bad), 1, wb, &g)); CHECK(wb.valid && g.nHours == 24 && g.fetchedEpoch == 5000);
    }
    // Мусор целиком: не разобрано вообще
    for (const char* bad : {"", "{}", "not json"}) { Forecast g = f; Weather wb; CHECK(!parseForecast(bad, std::strlen(bad), 1, wb, &g)); CHECK(g.nHours == 24 && g.fetchedEpoch == 5000); }
  }
  // --- Таблица WMO: все коды из документации Open-Meteo покрыты во всех языках ---
  for (int code : {0,1,2,3,45,48,51,53,55,56,57,61,63,65,66,67,71,73,75,77,80,81,82,85,86,95,96,99}) {
    for (Lang l : {Lang::En, Lang::Ru, Lang::De}) CHECK(description(l, code)[0] != '\0');
    CHECK(iconFor(code, true) != Icon::Unknown);
  }
  CHECK(iconFor(0, false) == Icon::ClearNight && iconFor(2, false) == Icon::PartlyCloudyNight && iconFor(63, false) == Icon::Rain);
  CHECK(iconFor(-1, true) == Icon::Unknown && iconFor(4, true) == Icon::Unknown && description(Lang::Ru, 4)[0] == '\0');
  // --- UTF-8: не рвём символ ---
  { char b[6]; copyUtf8(b, sizeof(b), "Москва"); CHECK(std::string(b) == "Мо"); }   // 2+2 байта + \0, третий символ не влезает
  { char b[5]; copyUtf8(b, sizeof(b), "Москва"); CHECK(std::string(b) == "Мо"); }
  // --- URL ---
  { char u[700]; int n = buildForecastUrl(55.7558, 37.6173, u, sizeof(u)); CHECK(n > 0 && n < (int)sizeof(u) && std::string(u).find("latitude=55.7558&longitude=37.6173") != std::string::npos);
    n = buildGeoUrl(Lang::Ru, u, sizeof(u)); CHECK(n > 0 && std::string(u).find("lang=ru") != std::string::npos); }
  // --- Поиск города (Open-Meteo Geocoding): настоящие ответы ---
  {
    const std::string j = slurp((dir + "/geocode_moskva_ru.json").c_str());
    GeoHit h[kMaxGeoHits];
    const int n = parseGeocode(j.data(), j.size(), h, kMaxGeoHits);
    CHECK(n == 4);
    CHECK(std::string(h[0].name) == "Москва" && std::string(h[0].region) == "Москва, Россия");
    CHECK(h[0].lat > 55.7 && h[0].lat < 55.8 && h[0].lon > 37.5 && h[0].lon < 37.7);
    CHECK(std::string(h[1].region) == "Айдахо, США" && h[1].lon < 0);
    const std::string b = slurp((dir + "/geocode_berlin_de.json").c_str());
    CHECK(parseGeocode(b.data(), b.size(), h, kMaxGeoHits) == 5 && std::string(h[0].name) == "Berlin");
    CHECK(parseGeocode(b.data(), b.size(), h, 2) == 2);  // не больше, чем просили
    const std::string none = slurp((dir + "/geocode_none.json").c_str());
    CHECK(parseGeocode(none.data(), none.size(), h, kMaxGeoHits) == 0);
    for (const char* bad : {"", "x", "[]", "{\"results\":"}) CHECK(parseGeocode(bad, std::strlen(bad), h, kMaxGeoHits) == -1);
    // место без координат или без названия пропускается; регион может быть пустым
    const char* partial = R"({"results":[{"name":"A"},{"name":"B","latitude":1.5,"longitude":2.5},{"latitude":3,"longitude":4}]})";
    CHECK(parseGeocode(partial, std::strlen(partial), h, kMaxGeoHits) == 1 && std::string(h[0].name) == "B" && h[0].region[0] == '\0');
  }
  // --- URL поиска: кириллица кодируется, пробел — %20, не влезло — -1 ---
  {
    char u[300];
    int n = buildGeocodeUrl("Нижний Новгород", Lang::Ru, u, sizeof(u));
    CHECK(n > 0 && std::string(u).find("name=%D0%9D%D0%B8%D0%B6%D0%BD%D0%B8%D0%B9%20%D0%9D") != std::string::npos);
    CHECK(std::string(u).find("&count=5&language=ru&format=json") != std::string::npos && n == (int)std::strlen(u));
    n = buildGeocodeUrl("St. Petersburg", Lang::En, u, sizeof(u));
    CHECK(n > 0 && std::string(u).find("name=St.%20Petersburg&") != std::string::npos);
    char tiny[40];
    CHECK(buildGeocodeUrl("Москва", Lang::Ru, tiny, sizeof(tiny)) == -1);
  }
  // --- Координаты текстом ---
  {
    double la = 0, lo = 0;
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    CHECK(parseCoords("55.75, 37.62", la, lo) && near(la, 55.75) && near(lo, 37.62));
    CHECK(parseCoords("55,75 37,62", la, lo) && near(la, 55.75) && near(lo, 37.62));
    CHECK(parseCoords("55.75;37.62", la, lo) && near(la, 55.75) && near(lo, 37.62));
    CHECK(parseCoords("55,75; 37,62", la, lo) && near(la, 55.75) && near(lo, 37.62));
    CHECK(parseCoords("55.75,37.62", la, lo) && near(la, 55.75) && near(lo, 37.62));
    CHECK(parseCoords(" -33.87  151.21 ", la, lo) && near(la, -33.87) && near(lo, 151.21));
    la = 1;
    lo = 2;
    for (const char* bad : {"", "Москва", "55.75", "91, 10", "10, 181", "55,75,37,62", "a, b", "55.75, 37.62x", "1 2 3"}) {
      CHECK(!parseCoords(bad, la, lo));
      CHECK(la == 1 && lo == 2);
    }
  }
  // --- Настройки: круг туда-обратно; пустой файл — значения из CalendarConfig.h; мусор — false ---
  {
    Settings s;
    s.autoLocation = false;
    s.sdLog = false;
    copyUtf8(s.manual.city, sizeof(s.manual.city), "Санкт-Петербург");
    s.manual.lat = 59.94;
    s.manual.lon = 30.31;
    const std::string j = serializeSettings(s);
    Settings r;
    CHECK(parseSettings(j.data(), j.size(), r));
    CHECK(!r.autoLocation && !r.sdLog && std::string(r.manual.city) == "Санкт-Петербург" && !r.manual.fromIp);
    CHECK(std::fabs(r.manual.lat - 59.94) < 1e-4 && std::fabs(r.manual.lon - 30.31) < 1e-4);
    Settings d;
    CHECK(parseSettings("{}", 2, d));
    CHECK(d.autoLocation == calendar_config::kLocationAutoByDefault && d.sdLog == calendar_config::kSdLogByDefault &&
          d.manual.lat == kDefaultLat && std::string(d.manual.city) == kDefaultCity);
    Settings keep;
    keep.sdLog = !calendar_config::kSdLogByDefault;
    for (const char* bad : {"", "x", "[]", R"({"manual":{"lat":95,"lon":0}})"}) {
      Settings t = keep;
      CHECK(!parseSettings(bad, std::strlen(bad), t));
      CHECK(t.sdLog == keep.sdLog);
    }
  }
  // --- Кэш: сеть геолокации и координаты погоды; в старом кэше погода относится к своему месту ---
  {
    Cache c;
    c.place.lat = 50.11;
    c.place.lon = 8.68;
    copyUtf8(c.place.ssid, sizeof(c.place.ssid), "Office-WiFi");
    c.weather.valid = true;
    c.weather.temp = 5;
    c.weather.atLat = 55.75;
    c.weather.atLon = 37.62;
    const std::string s = serializeCache(c);
    Cache r;
    CHECK(parseCache(s.data(), s.size(), r));
    CHECK(std::string(r.place.ssid) == "Office-WiFi" && std::fabs(r.weather.atLat - 55.75) < 1e-4 &&
          std::fabs(r.weather.atLon - 37.62) < 1e-4);
    const char* old = R"({"v":2,"place":{"lat":10.5,"lon":20.5,"city":"X","ip":1},"wx":{"t":3,"at":7}})";
    Cache o;
    CHECK(parseCache(old, std::strlen(old), o));
    CHECK(o.weather.atLat == 10.5 && o.weather.atLon == 20.5 && o.place.ssid[0] == '\0');
    CHECK(samePlace(55.75, 37.62, 55.76, 37.60) && !samePlace(55.75, 37.62, 55.85, 37.62) && !samePlace(NAN, 0, 0, 0));
  }
  // --- MET Norway: значки → WMO ---
  {
    CHECK(metSymbolToWmo("clearsky_day") == 0 && metSymbolToWmo("clearsky_night") == 0 && metSymbolToWmo("fair_polartwilight") == 1);
    CHECK(metSymbolToWmo("partlycloudy_night") == 2 && metSymbolToWmo("cloudy") == 3 && metSymbolToWmo("fog") == 45);
    CHECK(metSymbolToWmo("lightrain") == 61 && metSymbolToWmo("rain") == 63 && metSymbolToWmo("heavyrain") == 65);
    CHECK(metSymbolToWmo("lightrainshowers_day") == 80 && metSymbolToWmo("rainshowers_night") == 81 && metSymbolToWmo("heavyrainshowers_day") == 82);
    CHECK(metSymbolToWmo("lightsnow") == 71 && metSymbolToWmo("snow") == 73 && metSymbolToWmo("heavysnow") == 75);
    CHECK(metSymbolToWmo("snowshowers_day") == 85 && metSymbolToWmo("heavysnowshowers_night") == 86);
    CHECK(metSymbolToWmo("lightsleet") == 68 && metSymbolToWmo("heavysleet") == 69 && metSymbolToWmo("lightsleetshowers_day") == 83 &&
          metSymbolToWmo("sleetshowers_night") == 84);
    CHECK(metSymbolToWmo("rainandthunder") == 95 && metSymbolToWmo("lightssleetshowersandthunder_day") == 95);  // опечатка MET
    CHECK(metSymbolToWmo("") == -1 && metSymbolToWmo(nullptr) == -1 && metSymbolToWmo("sandstorm") == -1);
    for (int code : {68, 69, 83, 84}) {
      for (Lang l : {Lang::En, Lang::Ru, Lang::De}) CHECK(description(l, code)[0] != '\0');
      CHECK(iconFor(code, true) == Icon::Snow);
    }
  }
  // --- MET Norway: настоящий ответ (Москва, 26.09.2026, первый шаг 13:00 UTC) ---
  {
    const std::string j = slurp((dir + "/metno_moscow_complete.json").c_str());
    const uint32_t t13 = static_cast<uint32_t>(calendar_core::daysFromCivil(2026, 9, 26)) * 86400u + 13 * 3600u;
    const uint32_t now = t13 + 34 * 60;  // 16:34 по Москве
    const int32_t off = 3 * 3600;
    Weather w;
    Forecast f;
    CHECK(parseMetNo(j.data(), j.size(), now, 55.752, 37.6178, off, w, &f));
    CHECK(w.valid && w.provider == Provider::MetNo && w.fetchedEpoch == now);
    CHECK(std::fabs(w.temp - 15.8f) < 0.01f && !std::isnan(w.feels) && !std::isnan(w.windMs));
    CHECK(w.code == 3 && w.isDay);  // «cloudy»; 16:34 — ещё день
    CHECK(!std::isnan(w.tMin) && !std::isnan(w.tMax) && w.tMin <= w.temp && w.temp <= w.tMax);
    CHECK(!std::isnan(w.precipMm) && w.precipMm >= 0 && w.precipProb == -1);  // вероятности для России у MET нет
    CHECK(f.valid && f.provider == Provider::MetNo && f.utcOffsetSec == off);
    CHECK(f.nHours == kFcHours && f.h[0].ts == t13);
    for (int i = 1; i < f.nHours; ++i) CHECK(f.h[i].ts == f.h[i - 1].ts + 3600);
    for (int i = 0; i < f.nHours; ++i) CHECK(!std::isnan(f.h[i].temp) && f.h[i].code >= 0 && !std::isnan(f.h[i].mm) && f.h[i].prob == -1);
    // 23:00 по Москве (20:00 UTC) — ночь; 10:00 по Москве завтра (07:00 UTC) — день.
    CHECK(!f.h[7].isDay && f.h[18].isDay);
    CHECK(f.nDays == kFcDays);
    for (int i = 0; i < f.nDays; ++i) {
      CHECK(static_cast<int64_t>(f.d[i].ts) + off == static_cast<int64_t>(calendar_core::daysFromCivil(2026, 9, 26) + i) * 86400);
      CHECK(!std::isnan(f.d[i].tMin) && !std::isnan(f.d[i].tMax) && f.d[i].tMin <= f.d[i].tMax);
      CHECK(!std::isnan(f.d[i].precipMm) && f.d[i].code >= 0 && !std::isnan(f.d[i].windMax));
    }
    // Мусор — не разобрано, прежние данные не тронуты.
    for (const char* bad : {"", "{}", "not json", R"({"properties":{"timeseries":[]}})",
                            R"({"properties":{"timeseries":[{"time":"2026-09-26T13:00:00Z","data":{"instant":{"details":{}}}}]}})"}) {
      Weather keep = w;
      Forecast kf = f;
      CHECK(!parseMetNo(bad, std::strlen(bad), now, 55.75, 37.6, off, keep, &kf));
      CHECK(keep.temp == w.temp && kf.nHours == f.nHours);
    }
    // Нет «ощущается» — считается по температуре, влажности и ветру (как у Open-Meteo).
    const char* noFeels = R"({"properties":{"timeseries":[{"time":"2026-01-10T12:00:00Z","data":{"instant":{"details":{"air_temperature":-10.0,"relative_humidity":80.0,"wind_speed":5.0}},"next_1_hours":{"summary":{"symbol_code":"snow"},"details":{"precipitation_amount":0.4}}}}]}})";
    const uint32_t jan = static_cast<uint32_t>(calendar_core::daysFromCivil(2026, 1, 10)) * 86400u + 12 * 3600u;
    Weather nf;
    CHECK(parseMetNo(noFeels, std::strlen(noFeels), jan + 60, 55.75, 37.6, off, nf));
    CHECK(nf.feels < -15.f && nf.feels > -20.f && nf.code == 73 && std::fabs(nf.precipMm - 0.4f) < 1e-4);
  }
  // --- Nominatim: настоящие ответы ---
  {
    const std::string j = slurp((dir + "/nominatim_moskva_ru.json").c_str());
    GeoHit h[kMaxGeoHits];
    // Москва-регион и Москва-город с одинаковой подписью — одна строка, с координатами города.
    CHECK(parseNominatim(j.data(), j.size(), h, kMaxGeoHits) == 1);
    CHECK(std::string(h[0].name) == "Москва" && std::string(h[0].region) == "Центральный федеральный округ, Россия");
    CHECK(h[0].lat > 55.74 && h[0].lat < 55.76 && h[0].lon > 37.6 && h[0].lon < 37.63);
    const std::string none = slurp((dir + "/nominatim_none.json").c_str());
    CHECK(parseNominatim(none.data(), none.size(), h, kMaxGeoHits) == 0);
    for (const char* bad : {"", "x", "{}", "[{"}) CHECK(parseNominatim(bad, std::strlen(bad), h, kMaxGeoHits) == -1);
    const char* two = R"([{"name":"A","lat":"1.5","lon":"2.5","display_name":"A, R1"},{"name":"B","lat":"x","lon":"1"},{"name":"C","lat":"3","lon":"4","display_name":"Other"}])";
    CHECK(parseNominatim(two, std::strlen(two), h, kMaxGeoHits) == 2 && std::string(h[0].region) == "R1" && std::string(h[1].region) == "Other");
    CHECK(parseNominatim(two, std::strlen(two), h, 1) == 1);
  }
  // --- URL запасных путей ---
  {
    char u[700];
    CHECK(buildForecastUrl(55.7558, 37.6173, u, sizeof(u), false) > 0 && std::string(u).rfind("http://api.open-meteo.com/v1/forecast?latitude=55.76&longitude=37.62&", 0) == 0);
    CHECK(buildForecastUrl(55.7558, 37.6173, u, sizeof(u), true, "previous-runs-api.open-meteo.com") > 0 &&
          std::string(u).rfind("https://previous-runs-api.open-meteo.com/v1/forecast?latitude=55.7558&longitude=37.6173&current=", 0) == 0);
    CHECK(std::string(routeName(Route::OpenMeteoMirror)).find("Open-Meteo") == 0);
    CHECK(buildMetNoUrl(55.75581, 37.61733, u, sizeof(u)) > 0 &&
          std::string(u) == "https://api.met.no/weatherapi/locationforecast/2.0/complete?lat=55.7558&lon=37.6173");
    CHECK(buildNominatimUrl("Нижний Новгород", Lang::Ru, u, sizeof(u)) > 0 &&
          std::string(u).find("q=%D0%9D%D0%B8%D0%B6%D0%BD%D0%B8%D0%B9%20") != std::string::npos &&
          std::string(u).find("&format=jsonv2&limit=5&featureType=settlement&accept-language=ru") != std::string::npos);
    CHECK(buildGeocodeUrl("Berlin", Lang::De, u, sizeof(u), false) > 0 && std::string(u).rfind("http://geocoding-api.open-meteo.com/", 0) == 0);
  }
  // --- Кэш: источник погоды, путь, осадки по часам ---
  {
    Cache c;
    c.weather.valid = true;
    c.weather.temp = 1;
    c.weather.provider = Provider::MetNo;
    c.fc.valid = true;
    c.fc.provider = Provider::MetNo;
    c.fc.nHours = 2;
    c.fc.h[0].ts = 100;
    c.fc.h[0].mm = 0.3f;
    c.fc.h[1].ts = 3700;
    c.route = Route::OpenMeteoMirror;
    c.routeAt = 12345;
    const std::string s = serializeCache(c);
    Cache r;
    CHECK(parseCache(s.data(), s.size(), r));
    CHECK(r.weather.provider == Provider::MetNo && r.fc.provider == Provider::MetNo && r.route == Route::OpenMeteoMirror && r.routeAt == 12345);
    CHECK(std::fabs(r.fc.h[0].mm - 0.3f) < 1e-4 && std::isnan(r.fc.h[1].mm));
    Cache d;  // по умолчанию — Open-Meteo, основной путь; в файле этих полей нет
    const std::string sd = serializeCache(d);
    CHECK(sd.find("route") == std::string::npos && sd.find("src") == std::string::npos);
    Cache rd;
    CHECK(parseCache(sd.data(), sd.size(), rd) && rd.route == Route::OpenMeteoHttps && rd.routeAt == 0);
    const char* junk = R"({"place":{"lat":1,"lon":2},"route":9,"routeAt":5})";
    CHECK(parseCache(junk, std::strlen(junk), rd) && rd.route == Route::OpenMeteoHttps);
    CHECK(std::string(providerName(Provider::MetNo)) == "MET Norway" && std::string(providerName(Provider::OpenMeteo)) == "Open-Meteo.com");
  }
  // --- Сохранённые места: новое — первым, повтор (≈ 5 км) заменяет, лишнее выпадает; круг через файл настроек ---
  {
    Settings st;
    auto mk = [](const char* n, double la, double lo) { Place p; copyUtf8(p.city, sizeof(p.city), n); p.lat = la; p.lon = lo; return p; };
    addSaved(st, mk("A", 10, 10));
    addSaved(st, mk("B", 20, 20));
    addSaved(st, mk("A2", 10.01, 10.01));  // то же место — новое название, наверх
    CHECK(st.nSaved == 2 && std::string(st.saved[0].city) == "A2" && std::string(st.saved[1].city) == "B");
    for (int i = 0; i < 10; ++i) addSaved(st, mk("X", 30 + i, 30));
    CHECK(st.nSaved == kMaxSavedPlaces && st.saved[0].lat == 39);
    removeSaved(st, 0);
    CHECK(st.nSaved == kMaxSavedPlaces - 1 && st.saved[0].lat == 38);
    removeSaved(st, 99);
    CHECK(st.nSaved == kMaxSavedPlaces - 1);
    addSaved(st, mk("Дача", 56.1, 38.2));
    const std::string js = serializeSettings(st);
    Settings back;
    CHECK(parseSettings(js.data(), js.size(), back));
    CHECK(back.nSaved == st.nSaved && std::string(back.saved[0].city) == "Дача" && std::fabs(back.saved[0].lat - 56.1) < 1e-4);
    const char* old = R"({"v":1,"auto":true})";  // старый файл без списка
    Settings o;
    CHECK(parseSettings(old, std::strlen(old), o) && o.nSaved == 0);
  }
  // --- История: архив Open-Meteo (настоящий ответ) и своя запись ---
  {
    const std::string j = slurp((dir + "/archive_moscow.json").c_str());
    HistDay d[7];
    const int n = parseArchive(j.data(), j.size(), 55.75, 37.62, 777, d, 7);
    CHECK(n == 7);
    CHECK(d[0].date == 20260919 && d[6].date == 20260925 && d[3].src == HistSource::Archive && d[0].savedAt == 777);
    CHECK(std::fabs(d[3].mm - 23.6f) < 1e-3 && d[3].code == 63 && d[3].tMin <= d[3].tMax && !std::isnan(d[3].wind));
    CHECK(parseArchive(j.data(), j.size(), 55.75, 37.62, 777, d, 3) == 3);
    for (const char* bad : {"", "x", "{}", R"({"error":true,"reason":"x"})"}) CHECK(parseArchive(bad, std::strlen(bad), 1, 2, 3, d, 7) == -1);
    const char* nulls = R"({"daily":{"time":["2026-09-26"],"temperature_2m_max":[null],"temperature_2m_min":[null]}})";
    CHECK(parseArchive(nulls, std::strlen(nulls), 1, 2, 3, d, 7) == 0);  // архив до этого дня ещё не дошёл

    HistStore hs;
    HistDay rec;
    rec.date = 20260920; rec.lat = 55.75f; rec.lon = 37.62f; rec.tMax = 1; rec.src = HistSource::Recorded; rec.savedAt = 1;
    hs.put(rec);
    CHECK(hs.find(20260920, 55.75, 37.62) && hs.find(20260920, 55.75, 37.62)->src == HistSource::Recorded);
    CHECK(!hs.find(20260920, 59.9, 30.3) && !hs.find(20260921, 55.75, 37.62));  // другое место, другой день
    HistDay arc = rec; arc.src = HistSource::Archive; arc.tMax = 2; arc.savedAt = 2;
    hs.put(arc);
    CHECK(hs.n == 1 && hs.find(20260920, 55.75, 37.62)->src == HistSource::Archive);
    hs.put(rec);  // запись устройства архив не заменяет
    CHECK(hs.find(20260920, 55.75, 37.62)->tMax == 2);
    for (int i = 0; i < kHistMax + 5; ++i) { HistDay x = rec; x.date = 20250101 + i; x.savedAt = 100 + i; hs.put(x); }
    CHECK(hs.n == kHistMax && !hs.find(20260920, 55.75, 37.62));  // самое давнее вытеснено
    const std::string sj = serializeHistory(hs);
    HistStore back;
    CHECK(parseHistory(sj.data(), sj.size(), back) && back.n == hs.n && back.d[0].date == hs.d[0].date);
    char u[400];
    CHECK(buildArchiveUrl(55.7558, 37.6173, 20260919, 20260925, u, sizeof(u)) > 0 &&
          std::string(u).find("https://archive-api.open-meteo.com/v1/archive?latitude=55.76&longitude=37.62&start_date=2026-09-19&end_date=2026-09-25") == 0);
  }
  std::printf("weather: ошибок %d\n", fails);
  return fails;
}
