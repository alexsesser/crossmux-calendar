#include "MoonPhase.h"

#include <cmath>

namespace moon_phase {

using calendar_core::Lang;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;
constexpr double kSynodic = 29.530588861;

double sind(double deg) { return std::sin(deg * kRad); }

// Meeus 49: JDE (TT) для k (целое — новолуние, целое + 0.5 — полнолуние).
double jde(double k, bool full) {
  const double T = k / 1236.85, T2 = T * T, T3 = T2 * T, T4 = T3 * T;
  double j = 2451550.09766 + kSynodic * k + 0.00015437 * T2 - 0.000000150 * T3 + 0.00000000073 * T4;
  const double E = 1.0 - 0.002516 * T - 0.0000074 * T2;
  const double M = 2.5534 + 29.10535670 * k - 0.0000014 * T2 - 0.00000011 * T3;
  const double Mp = 201.5643 + 385.81693528 * k + 0.0107582 * T2 + 0.00001238 * T3 - 0.000000058 * T4;
  const double F = 160.7108 + 390.67050284 * k - 0.0016118 * T2 - 0.00000227 * T3 + 0.000000011 * T4;
  const double Om = 124.7746 - 1.56375588 * k + 0.0020672 * T2 + 0.00000215 * T3;

  double c = 0;
  c += (full ? -0.40614 : -0.40720) * sind(Mp);
  c += (full ? 0.17302 : 0.17241) * E * sind(M);
  c += (full ? 0.01614 : 0.01608) * sind(2 * Mp);
  c += (full ? 0.01043 : 0.01039) * sind(2 * F);
  c += (full ? 0.00734 : 0.00739) * E * sind(Mp - M);
  c += (full ? -0.00515 : -0.00514) * E * sind(Mp + M);
  c += (full ? 0.00209 : 0.00208) * E * E * sind(2 * M);
  c += -0.00111 * sind(Mp - 2 * F);
  c += -0.00057 * sind(Mp + 2 * F);
  c += 0.00056 * E * sind(2 * Mp + M);
  c += -0.00042 * sind(3 * Mp);
  c += 0.00042 * E * sind(M + 2 * F);
  c += 0.00038 * E * sind(M - 2 * F);
  c += -0.00024 * E * sind(2 * Mp - M);
  c += -0.00017 * sind(Om);
  c += -0.00007 * sind(Mp + 2 * M);
  c += 0.00004 * sind(2 * Mp - 2 * F);
  c += 0.00004 * sind(3 * M);
  c += 0.00003 * sind(Mp + M - 2 * F);
  c += 0.00003 * sind(2 * Mp + 2 * F);
  c += -0.00003 * sind(Mp + M + 2 * F);
  c += 0.00003 * sind(Mp - M + 2 * F);
  c += -0.00002 * sind(Mp - M - 2 * F);
  c += -0.00002 * sind(3 * Mp + M);
  c += 0.00002 * sind(4 * Mp);

  // Планетные поправки (до 28 с).
  c += 0.000325 * sind(299.77 + 0.107408 * k - 0.009173 * T2);
  c += 0.000165 * sind(251.88 + 0.016321 * k);
  c += 0.000164 * sind(251.83 + 26.651886 * k);
  c += 0.000126 * sind(349.42 + 36.412478 * k);
  c += 0.000110 * sind(84.66 + 18.206239 * k);
  c += 0.000062 * sind(141.74 + 53.303771 * k);
  c += 0.000060 * sind(207.14 + 2.453732 * k);
  c += 0.000056 * sind(154.84 + 7.306860 * k);
  c += 0.000047 * sind(34.52 + 27.261239 * k);
  c += 0.000042 * sind(207.19 + 0.121824 * k);
  c += 0.000040 * sind(291.34 + 1.844379 * k);
  c += 0.000037 * sind(161.72 + 24.198154 * k);
  c += 0.000035 * sind(239.56 + 25.513099 * k);
  c += 0.000023 * sind(331.55 + 3.592518 * k);
  return j + c;
}

// TT − UT (секунды), аппроксимация Espenak–Meeus для 1961…2150. Нужна, чтобы «истинное» время события (TT)
// перевести в UT; без неё к 2100 г. накопилось бы ~2 минуты ошибки.
double deltaT(double year) {
  if (year < 1986) {
    const double t = year - 1975;
    return 45.45 + 1.067 * t - t * t / 260.0 - t * t * t / 718.0;
  }
  if (year < 2005) {
    const double t = year - 2000;
    return 63.86 + 0.3345 * t - 0.060374 * t * t + 0.0017275 * t * t * t + 0.000651814 * t * t * t * t +
           0.00002373599 * t * t * t * t * t;
  }
  if (year < 2050) {
    const double t = year - 2000;
    return 62.92 + 0.32217 * t + 0.005589 * t * t;
  }
  const double u = (year - 1820) / 100.0;
  return -20.0 + 32.0 * u * u - 0.5628 * (2150.0 - year);
}

double toUt(double jdeTt) {
  const double year = 2000.0 + (jdeTt - 2451545.0) / 365.2425;
  return jdeTt - deltaT(year) / 86400.0;
}

Date dateFromJd(double jdUt, int off) {
  const double localDays = jdUt - 2440587.5 + off / 1440.0;
  int y;
  unsigned m, d;
  calendar_core::civilFromDays(static_cast<int32_t>(std::floor(localDays)), y, m, d);
  return Date{y, m, d};
}

int kFor(double jdUt) { return static_cast<int>(std::floor((jdUt - 2451550.09766) / kSynodic)); }

}  // namespace

double eventJdUt(int k, bool full) { return toUt(jde(k + (full ? 0.5 : 0.0), full)); }

double julianFromLocal(int y, unsigned m, unsigned d, int hh, int mm, int off) {
  const double days = static_cast<double>(calendar_core::daysFromCivil(y, m, d));
  return days + 2440587.5 + (hh * 60 + mm - off) / 1440.0;
}

Info at(double jd) {
  const int k = kFor(jd);
  double last = eventJdUt(k, false), next = eventJdUt(k + 1, false);
  int kk = k;
  if (last > jd) {  // страховка на границе
    --kk;
    next = last;
    last = eventJdUt(kk, false);
  } else if (next <= jd) {
    ++kk;
    last = next;
    next = eventJdUt(kk + 1, false);
  }
  Info r;
  r.fraction = (jd - last) / (next - last);
  r.illum = (1.0 - std::cos(2.0 * kPi * r.fraction)) / 2.0;
  r.lunarDay = static_cast<int>(std::floor(r.fraction * kSynodic)) + 1;
  const double f = r.fraction;
  r.phaseIdx = (f < 0.03 || f >= 0.97) ? 0 : f < 0.22 ? 1 : f < 0.28 ? 2 : f < 0.47 ? 3 : f < 0.53 ? 4 : f < 0.72 ? 5 : f < 0.78 ? 6 : 7;
  return r;
}

Date nextNewMoon(double jd, int off) {
  for (int k = kFor(jd) - 1; k < kFor(jd) + 3; ++k) {
    const double t = eventJdUt(k, false);
    if (t > jd) return dateFromJd(t, off);
  }
  return dateFromJd(jd, off);
}

Date nextFullMoon(double jd, int off) {
  for (int k = kFor(jd) - 1; k < kFor(jd) + 3; ++k) {
    const double t = eventJdUt(k, true);
    if (t > jd) return dateFromJd(t, off);
  }
  return dateFromJd(jd, off);
}

const char* phaseName(Lang lang, int i) {
  static const char* const en[8] = {"New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
                                    "Full moon", "Waning gibbous", "Last quarter", "Waning crescent"};
  static const char* const ru[8] = {"Новолуние", "Растущий серп", "Первая четверть", "Растущая луна",
                                    "Полнолуние", "Убывающая луна", "Последняя четверть", "Убывающий серп"};
  static const char* const de[8] = {"Neumond", "Zunehmende Sichel", "Erstes Viertel", "Zunehmender Mond",
                                    "Vollmond", "Abnehmender Mond", "Letztes Viertel", "Abnehmende Sichel"};
  i = i < 0 ? 0 : i > 7 ? 7 : i;
  return lang == Lang::Ru ? ru[i] : lang == Lang::De ? de[i] : en[i];
}

}  // namespace moon_phase
