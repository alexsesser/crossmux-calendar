#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "CalendarConfig.h"
#include "CalendarCore.h"

// Чистая часть погоды: типы, разбор ответов ipwhois.app и Open-Meteo, таблица кодов WMO,
// (де)сериализация кэша. Без Arduino/SDK; ArduinoJson — header-only, тестируется на хосте.
namespace weather_core {

// Место по умолчанию — из CalendarConfig.h: показывается, пока IP-геолокация ни разу не удалась.
constexpr double kDefaultLat = calendar_config::kDefaultLatitude;
constexpr double kDefaultLon = calendar_config::kDefaultLongitude;
constexpr const char* kDefaultCity = calendar_config::kDefaultCity;

void copyUtf8(char* dst, size_t dstSize, const char* src);

struct Place {
  double lat = kDefaultLat;
  double lon = kDefaultLon;
  char city[48];              // пустая строка — название неизвестно
  bool fromIp = false;        // false — значение по умолчанию, IP-определение ещё не удавалось
  uint32_t ipEpoch = 0;       // когда место определено по IP
  uint8_t ipLang = 0;         // на каком языке получено название (calendar_core::Lang)
  Place() { copyUtf8(city, sizeof(city), kDefaultCity); }
};

// Поле, которого нет, — NaN: интерфейс рисует заглушку именно для него, а не для всего блока.
struct Weather {
  bool valid = false;         // есть хотя бы температура
  float temp = NAN;
  float feels = NAN;
  float tMin = NAN;
  float tMax = NAN;
  float windMs = NAN;
  float precipMm = NAN;
  int16_t precipProb = -1;    // % (-1 — нет данных)
  int16_t code = -1;          // WMO (-1 — нет данных)
  bool isDay = true;
  uint32_t fetchedEpoch = 0;
};

struct Cache {
  Place place;
  Weather weather;
};

enum class Icon : uint8_t {
  Clear, ClearNight, PartlyCloudy, PartlyCloudyNight, Cloudy, Fog, Drizzle, Rain, Snow, Thunder, Unknown
};

Icon iconFor(int code, bool isDay);
// Текст для кода WMO; для неизвестного кода — пустая строка.
const char* description(calendar_core::Lang lang, int code);

struct Labels {
  const char* min;
  const char* max;
  const char* wind;
  const char* precip;
  const char* feels;
  const char* updated;      // «обн.»
  const char* stale;        // «устарело»
  const char* noData;       // «Нет данных»
  const char* unknownPlace;
  const char* windUnit;     // «м/с»
  const char* mmUnit;       // «мм»
};
const Labels& labels(calendar_core::Lang lang);

// Данные старше этого — не показываем значения (заглушки); старше kStaleSec — помечаем «устарело».
constexpr uint32_t kStaleSec = calendar_config::kStaleHours * 3600u;
constexpr uint32_t kExpireSec = calendar_config::kExpireHours * 3600u;

int buildGeoUrl(calendar_core::Lang lang, char* buf, size_t size);
int buildForecastUrl(double lat, double lon, char* buf, size_t size);

// ipwhois.app: {"success":true,"city":"..","latitude":..,"longitude":..}. out меняется только при true.
bool parseGeo(const char* json, size_t len, calendar_core::Lang lang, uint32_t nowEpoch, Place& out);
// Open-Meteo forecast (current + daily). out меняется только при true (есть температура).
bool parseForecast(const char* json, size_t len, uint32_t nowEpoch, Weather& out);

std::string serializeCache(const Cache& c);
bool parseCache(const char* json, size_t len, Cache& out);  // при false out не меняется

// copyUtf8 (объявлена выше): обрезка UTF-8 до dstSize-1 байт по границе символа + завершающий ноль.

}  // namespace weather_core
