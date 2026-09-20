#include "SunTimes.h"

#include <cmath>

#include "CalendarCore.h"

namespace sun_times {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
// Зенит для восхода/заката: 90°50' (радиус диска + рефракция).
constexpr double kZenith = 90.833;

double julianCentury(double jd) { return (jd - 2451545.0) / 36525.0; }

struct Sun {
  double eqTimeMin;
  double declRad;
};

Sun solarPosition(double t) {
  const double l0 = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
  const double m = 357.52911 + t * (35999.05029 - 0.0001537 * t);
  const double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
  const double mr = m * kDeg;
  const double c = std::sin(mr) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                   std::sin(2 * mr) * (0.019993 - 0.000101 * t) + std::sin(3 * mr) * 0.000289;
  const double trueLong = l0 + c;
  const double omega = 125.04 - 1934.136 * t;
  const double lambda = trueLong - 0.00569 - 0.00478 * std::sin(omega * kDeg);
  const double eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
  const double eps = eps0 + 0.00256 * std::cos(omega * kDeg);
  const double decl = std::asin(std::sin(eps * kDeg) * std::sin(lambda * kDeg));

  double y = std::tan(eps * kDeg / 2.0);
  y *= y;
  const double l0r = l0 * kDeg;
  const double eq =
      y * std::sin(2 * l0r) - 2 * e * std::sin(mr) + 4 * e * y * std::sin(mr) * std::cos(2 * l0r) -
      0.5 * y * y * std::sin(4 * l0r) - 1.25 * e * e * std::sin(2 * mr);
  return {4.0 * eq / kDeg, decl};
}

// cos часового угла; за пределами [-1,1] солнце не пересекает горизонт.
double cosHourAngle(double latRad, double declRad) {
  return std::cos(kZenith * kDeg) / (std::cos(latRad) * std::cos(declRad)) - std::tan(latRad) * std::tan(declRad);
}

// UTC-минуты события от полуночи UTC-даты jd0; два прохода (второй — по солнцу в момент события).
double eventUtcMin(bool rise, double jd0, double latDeg, double lonDeg, bool& polarDay, bool& polarNight) {
  double minutes = 720.0;
  for (int pass = 0; pass < 2; ++pass) {
    const Sun s = solarPosition(julianCentury(jd0 + minutes / 1440.0));
    const double ch = cosHourAngle(latDeg * kDeg, s.declRad);
    if (ch > 1.0) {
      polarNight = true;
      return 0;
    }
    if (ch < -1.0) {
      polarDay = true;
      return 0;
    }
    const double ha = std::acos(ch) / kDeg;  // градусы, > 0
    minutes = 720.0 - 4.0 * (lonDeg + (rise ? ha : -ha)) - s.eqTimeMin;
  }
  return minutes;
}

int wrapDay(double minutes, int utcOffsetMin) {
  int m = static_cast<int>(std::lround(minutes)) + utcOffsetMin;
  m %= 1440;
  return m < 0 ? m + 1440 : m;
}

}  // namespace

Result compute(int year, unsigned month, unsigned day, double latDeg, double lonDeg, int utcOffsetMin) {
  Result r{};
  if (month < 1 || month > 12 || day < 1 || day > calendar_core::daysInMonth(year, month) || latDeg < -90.0 ||
      latDeg > 90.0 || lonDeg < -180.0 || lonDeg > 180.0) {
    return r;
  }
  r.valid = true;
  // Опорная дата — местная. Полночь UTC этой даты; для дальних поясов ошибка склонения < 1 мин.
  const double jd0 = static_cast<double>(calendar_core::daysFromCivil(year, month, day)) + 2440587.5;

  const double rise = eventUtcMin(true, jd0, latDeg, lonDeg, r.polarDay, r.polarNight);
  const double set = eventUtcMin(false, jd0, latDeg, lonDeg, r.polarDay, r.polarNight);
  if (r.polarDay || r.polarNight) {
    r.daylightMin = r.polarDay ? 1440 : 0;
    return r;
  }
  r.sunriseMin = wrapDay(rise, utcOffsetMin);
  r.sunsetMin = wrapDay(set, utcOffsetMin);
  r.daylightMin = ((r.sunsetMin - r.sunriseMin) % 1440 + 1440) % 1440;
  return r;
}

}  // namespace sun_times
