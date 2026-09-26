#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "CalendarConfig.h"
#include "CalendarCore.h"

// Чистая часть погоды: типы, разбор ответов ipwhois.app, Open-Meteo, MET Norway и Nominatim, таблица кодов WMO,
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
  char ssid[33];              // через какую Wi-Fi-сеть определено по IP: сеть сменилась (дом ⇄ работа) — определяем заново
  Place() {
    copyUtf8(city, sizeof(city), kDefaultCity);
    ssid[0] = '\0';
  }
};

// Координаты рядом (≈ 5 км) — одно и то же место для погоды.
bool samePlace(double lat1, double lon1, double lat2, double lon2);

// Откуда данные о погоде: на экране «Погода» подписан источник (требование лицензий CC BY 4.0 обоих).
enum class Provider : uint8_t { OpenMeteo = 0, MetNo = 1 };
const char* providerName(Provider p);  // «Open-Meteo.com» / «MET Norway»

// Пути к погоде — пока какой-то не сработает (§13.9–13.10 концепции): Open-Meteo по HTTPS (сервер, который даёт DNS);
// другие серверы Open-Meteo по IP (kOpenMeteoAltServers, обычный HTTP); тот же сервер обычным HTTP (если мешают только
// шифрованию); MET Norway. Сработавший путь запоминается и пробуется первым в следующий раз.
enum class Route : uint8_t { OpenMeteoHttps = 0, OpenMeteoHttp = 1, MetNo = 2, OpenMeteoAlt = 3 };
constexpr int kRoutes = 4;
const char* routeName(Route r);

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
  // Для каких координат получена: место сменилось (ручное ⇄ по IP, другой город) — эти данные не показываем.
  double atLat = NAN;
  double atLon = NAN;
  Provider provider = Provider::OpenMeteo;
};

// Прогноз для подробных экранов: ближайшие 24 часа и 7 дней (сегодня + 6). Отсутствующие поля — NaN / -1.
constexpr int kFcHours = 24;
constexpr int kFcDays = 7;

struct FcHour {
  uint32_t ts = 0;  // Unix, начало часа
  float temp = NAN;
  float wind = NAN;  // м/с
  int16_t code = -1;
  int8_t prob = -1;  // вероятность осадков, %
  float mm = NAN;    // осадки за этот час, мм (есть у MET Norway; у Open-Meteo вместо них — вероятность)
  bool isDay = true;
};

struct FcDay {
  uint32_t ts = 0;  // Unix, начало суток по местному времени места
  float tMax = NAN;
  float tMin = NAN;
  float precipMm = NAN;
  float windMax = NAN;
  int16_t code = -1;
  int8_t prob = -1;
};

struct Forecast {
  bool valid = false;  // есть хотя бы один час или день
  uint8_t nHours = 0;
  uint8_t nDays = 0;
  int32_t utcOffsetSec = 0;  // смещение места (для локальных дат и часов)
  uint32_t fetchedEpoch = 0;
  Provider provider = Provider::OpenMeteo;
  FcHour h[kFcHours];
  FcDay d[kFcDays];
};

struct Cache {
  Place place;
  Weather weather;
  Forecast fc;
  Route route = Route::OpenMeteoHttps;  // каким путём погода пришла в последний раз — его пробуем первым
  uint32_t routeAt = 0;                 // с какого момента этот путь первый (через kWeatherPrimaryRetryMin — снова Open-Meteo)
};

enum class Icon : uint8_t {
  Clear, ClearNight, PartlyCloudy, PartlyCloudyNight, Cloudy, Fog, Drizzle, Rain, Snow, Thunder, Unknown
};

Icon iconFor(int code, bool isDay);
// Текст для кода WMO; для неизвестного кода — пустая строка.
const char* description(calendar_core::Lang lang, int code);

struct Labels {
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
// https=false — обычный HTTP; координаты тогда округляются до 0,01° (≈ 1 км: точнее для погоды не нужно, а запрос
// идёт открытым текстом).
// host — другой сервер (IP) вместо api.open-meteo.com; только для https=false (заголовок Host ставит клиент).
int buildForecastUrl(double lat, double lon, char* buf, size_t size, bool https = true, const char* host = nullptr);
// MET Norway Locationforecast 2.0 «complete» (без ключа; нужен User-Agent с контактом — kHttpUserAgent).
int buildMetNoUrl(double lat, double lon, char* buf, size_t size);

// ipwhois.app: {"success":true,"city":"..","latitude":..,"longitude":..}. out меняется только при true.
bool parseGeo(const char* json, size_t len, calendar_core::Lang lang, uint32_t nowEpoch, Place& out);
// Open-Meteo forecast (current + daily). out меняется только при true (есть температура).
// Если задан detail — из того же разбора заполняется и прогноз 24 ч + 7 дней (он не обязателен: при его отсутствии
// в ответе detail не меняется, а функция всё равно возвращает true).
bool parseForecast(const char* json, size_t len, uint32_t nowEpoch, Weather& out, Forecast* detail = nullptr);
// MET Norway: то же самое, но из их ряда прогнозов (UTC, шаг 1 ч на ≈ 2,5 суток, дальше 6 ч). Часовой пояс места
// сервис не сообщает — берём utcOffsetSec (часы устройства); день/ночь — по восходу/закату (SunTimes) для lat/lon.
bool parseMetNo(const char* json, size_t len, uint32_t nowEpoch, double lat, double lon, int32_t utcOffsetSec,
                Weather& out, Forecast* detail = nullptr);
// Значок MET Norway («lightrainshowers_day», «heavysnowandthunder» …) → код WMO; -1 — не знаем.
int metSymbolToWmo(const char* symbol);

std::string serializeCache(const Cache& c);
bool parseCache(const char* json, size_t len, Cache& out);  // при false out не меняется

// ---- Настройки, которые меняются на устройстве (экран «Место») ----------------------------------------------------
// Лежат на SD отдельно от кэша (удалить кэш — настройки останутся). Начальные значения — из CalendarConfig.h.
struct Settings {
  bool autoLocation = calendar_config::kLocationAutoByDefault;  // true — место по IP, false — вручную
  Place manual;                                               // место «вручную»; по умолчанию — kDefaultCity
  bool sdLog = calendar_config::kSdLogByDefault;              // подробный журнал на SD-карту
};
std::string serializeSettings(const Settings& s);
bool parseSettings(const char* json, size_t len, Settings& out);  // при false out не меняется

// ---- Поиск города по названию: Open-Meteo Geocoding API (без ключа, CC BY 4.0) ------------------------------------
struct GeoHit {
  char name[48];    // «Москва»
  char region[80];  // «Москва, Россия» / «Айдахо, США»
  double lat = 0, lon = 0;
};
constexpr int kMaxGeoHits = 5;
// Длина URL или -1, если не влезло. Запрос кодируется (UTF-8 → %XX), язык названий — как у интерфейса.
int buildGeocodeUrl(const char* query, calendar_core::Lang lang, char* buf, size_t size, bool https = true);
// Число найденных мест (0 — ничего не найдено) или -1 — ответ не разобрать.
int parseGeocode(const char* json, size_t len, GeoHit* out, int maxOut);
// Запасной поиск — OpenStreetMap Nominatim (без ключа, ODbL; нужен User-Agent с контактом): только населённые пункты.
int buildNominatimUrl(const char* query, calendar_core::Lang lang, char* buf, size_t size);
int parseNominatim(const char* json, size_t len, GeoHit* out, int maxOut);
// Координаты текстом: «55.75, 37.62», «55,75 37,62», «55.75;37.62». false — это не координаты (или вне диапазона).
bool parseCoords(const char* text, double& lat, double& lon);

// copyUtf8 (объявлена выше): обрезка UTF-8 до dstSize-1 байт по границе символа + завершающий ноль.

}  // namespace weather_core
