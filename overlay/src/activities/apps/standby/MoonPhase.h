#pragma once

#include "CalendarCore.h"

// Фазы Луны по Meeus, «Astronomical Algorithms», гл. 49 (истинные новолуния и полнолуния, точность — секунды/минуты).
// Чистая логика без Arduino. Момент — юлианская дата в UT (JD); переводы из/в местное время — ниже.
namespace moon_phase {

// JD (UT) 12:00 местной даты + смещение часов (минуты от UTC).
double julianFromLocal(int year, unsigned month, unsigned day, int hour, int minute, int utcOffsetMin);

struct Info {
  double fraction;  // положение в цикле 0..1 (0 — новолуние, 0.5 — полнолуние)
  double illum;     // освещённая доля диска 0..1
  int lunarDay;     // «лунный день» 1..30
  int phaseIdx;     // 0 новолуние, 1 растущий серп, 2 первая четверть, 3 растущая, 4 полнолуние, 5 убывающая, 6 последняя четверть, 7 убывающий серп
};
Info at(double jdUt);

struct Date {
  int year;
  unsigned month, day;
};
// Ближайшие новолуние/полнолуние ПОСЛЕ момента jdUt; дата — по местному времени со смещением utcOffsetMin.
Date nextNewMoon(double jdUt, int utcOffsetMin);
Date nextFullMoon(double jdUt, int utcOffsetMin);

// JD (UT) k-го новолуния (full=false) или полнолуния (full=true); k — целое, отсчёт от 2000-01-06. Нужен тестам.
double eventJdUt(int k, bool full);

const char* phaseName(calendar_core::Lang lang, int phaseIdx);

}  // namespace moon_phase
