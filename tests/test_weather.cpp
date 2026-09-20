// Тесты WeatherCore: настоящие ответы API (tests/data) + негативные случаи. Печатает FAIL-строки; код возврата = число ошибок.
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
    const std::string j = slurp((dir + "/openmeteo_moscow.json").c_str());
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
  { char u[400]; int n = buildForecastUrl(55.7558, 37.6173, u, sizeof(u)); CHECK(n > 0 && n < (int)sizeof(u) && std::string(u).find("latitude=55.7558&longitude=37.6173") != std::string::npos);
    n = buildGeoUrl(Lang::Ru, u, sizeof(u)); CHECK(n > 0 && std::string(u).find("lang=ru") != std::string::npos); }
  std::printf("weather: ошибок %d\n", fails);
  return fails;
}
