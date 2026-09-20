#pragma once

// Восход/закат по алгоритму NOAA Solar Calculator. Офлайн, без сети. Чистая логика (без Arduino).
namespace sun_times {

struct Result {
  bool valid;         // false — координаты вне диапазона
  bool polarDay;      // солнце не заходит
  bool polarNight;    // солнце не восходит
  int sunriseMin;     // местные минуты от полуночи, 0..1439 (только если !polar*)
  int sunsetMin;
  int daylightMin;    // длина дня в минутах (полярный день = 1440, ночь = 0)
};

// latDeg: +север, lonDeg: +восток; utcOffsetMin — смещение местного времени от UTC в минутах.
Result compute(int year, unsigned month, unsigned day, double latDeg, double lonDeg, int utcOffsetMin);

}  // namespace sun_times
