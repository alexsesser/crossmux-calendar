#pragma once

#include <cstdint>

// Чистая логика григорианского календаря для CalendarFace.
// Без Arduino/SDK: компилируется и тестируется на хосте (tests/run.sh).
namespace calendar_core {

constexpr int kMinYear = 1970;
constexpr int kMaxYear = 2100;

bool isLeap(int year);
unsigned daysInMonth(int year, unsigned month);  // month 1..12
unsigned daysInYear(int year);

// Дней с 1970-01-01 (может быть отрицательным до 1970).
int32_t daysFromCivil(int year, unsigned month, unsigned day);

// Обратное к daysFromCivil: дни с 1970-01-01 → дата.
void civilFromDays(int32_t days, int& year, unsigned& month, unsigned& day);

// 0 = понедельник … 6 = воскресенье.
unsigned weekday(int year, unsigned month, unsigned day);

// Порядковый день в году, 1..366.
unsigned dayOfYear(int year, unsigned month, unsigned day);

// Номер недели ISO-8601 (1..53); isoYear — год, которому неделя принадлежит.
unsigned isoWeek(int year, unsigned month, unsigned day, int* isoYear = nullptr);

// Сдвиг месяца на delta в пределах [kMinYear, kMaxYear]. false — выход за границы,
// outYear/outMonth не меняются.
bool addMonths(int year, unsigned month, int delta, int& outYear, unsigned& outMonth);

enum class CellKind : uint8_t { PrevMonth, CurrentMonth, NextMonth };

struct Cell {
  int16_t year;
  uint8_t month;
  uint8_t day;
  CellKind kind;
  bool weekend;  // сб/вс
};

constexpr int kGridCols = 7;
constexpr int kGridMaxRows = 6;

struct MonthGrid {
  Cell cells[kGridCols * kGridMaxRows];
  uint8_t rows;  // 4..6: только строки, в которых есть дни месяца
};

// Сетка месяца, неделя с понедельника. Дни соседних месяцев заполняют края.
void buildMonthGrid(int year, unsigned month, MonthGrid& out);

// ---- Названия (свои таблицы, i18n upstream не трогаем) ----------------------

enum class Lang : uint8_t { En, Ru, De };

struct Labels {
  const char* week;       // «Нед.»
  const char* day;        // «День»
  const char* hoursShort; // «ч»
  const char* minutesShort;
  const char* polarDay;
  const char* polarNight;
  const char* noClock;    // «Время не задано»
};

const Labels& labels(Lang lang);
const char* weekdayName(Lang lang, unsigned wd);       // wd 0=Пн, полное
const char* weekdayShort(Lang lang, unsigned wd);      // wd 0=Пн, 2 буквы
const char* monthName(Lang lang, unsigned month);      // именительный, 1..12
const char* monthNameGenitive(Lang lang, unsigned month);  // «сентября»

// «20 сентября 2026» / «20. September 2026» / «September 20, 2026».
int formatLongDate(Lang lang, int year, unsigned month, unsigned day, char* buf, unsigned size);
// Смещение часов от UTC: «UTC+3», «UTC+5:30», «UTC-3:30», «UTC+0». Минуты — со знаком.
int formatUtcOffset(int offsetMinutes, char* buf, unsigned size);

// «20.09.2026» (en: «2026-09-20»).
int formatShortDate(Lang lang, int year, unsigned month, unsigned day, char* buf, unsigned size);

}  // namespace calendar_core
