#include "CalendarCore.h"

#include <cstdio>

namespace calendar_core {

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned daysInMonth(int y, unsigned m) {
  static constexpr uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  return (m == 2 && isLeap(y)) ? 29u : kDays[m - 1];
}

unsigned daysInYear(int y) { return isLeap(y) ? 366u : 365u; }

// Алгоритм days_from_civil (H. Hinnant), эпоха 1970-01-01.
int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

unsigned weekday(int y, unsigned m, unsigned d) {
  // 1970-01-01 — четверг (пн = 0 → чт = 3).
  const int32_t z = daysFromCivil(y, m, d) + 3;
  const int32_t r = z % 7;
  return static_cast<unsigned>(r < 0 ? r + 7 : r);
}

unsigned dayOfYear(int y, unsigned m, unsigned d) {
  return static_cast<unsigned>(daysFromCivil(y, m, d) - daysFromCivil(y, 1, 1)) + 1u;
}

namespace {

// Количество ISO-недель в году: 53, если 1 января — четверг, либо (високосный и среда).
unsigned isoWeeksInYear(int y) {
  const unsigned jan1 = weekday(y, 1, 1);  // 0=Пн
  return (jan1 == 3 || (isLeap(y) && jan1 == 2)) ? 53u : 52u;
}

}  // namespace

unsigned isoWeek(int y, unsigned m, unsigned d, int* isoYear) {
  const int wd = static_cast<int>(weekday(y, m, d)) + 1;  // ISO: 1=Пн … 7=Вс
  int week = (static_cast<int>(dayOfYear(y, m, d)) - wd + 10) / 7;
  int wy = y;
  if (week < 1) {
    wy = y - 1;
    week = static_cast<int>(isoWeeksInYear(wy));
  } else if (week > static_cast<int>(isoWeeksInYear(y))) {
    wy = y + 1;
    week = 1;
  }
  if (isoYear) *isoYear = wy;
  return static_cast<unsigned>(week);
}

bool addMonths(int year, unsigned month, int delta, int& outYear, unsigned& outMonth) {
  const int idx = year * 12 + static_cast<int>(month) - 1 + delta;
  const int ny = idx / 12;
  const int nm = idx % 12 + 1;
  if (ny < kMinYear || ny > kMaxYear) return false;
  outYear = ny;
  outMonth = static_cast<unsigned>(nm);
  return true;
}

void buildMonthGrid(int year, unsigned month, MonthGrid& out) {
  const unsigned lead = weekday(year, month, 1);  // сколько дней предыдущего месяца слева
  const unsigned dim = daysInMonth(year, month);
  out.rows = static_cast<uint8_t>((lead + dim + kGridCols - 1) / kGridCols);

  int py = year;
  unsigned pm = 0;
  if (!addMonths(year, month, -1, py, pm)) {
    py = year - 1;
    pm = 12;  // до 1970 только для отрисовки серых чисел
  }
  const unsigned pdim = daysInMonth(py, pm);
  int ny = year;
  unsigned nm = 0;
  if (!addMonths(year, month, +1, ny, nm)) {
    ny = year + 1;
    nm = 1;
  }

  const unsigned total = static_cast<unsigned>(out.rows) * kGridCols;
  for (unsigned i = 0; i < total; ++i) {
    Cell& c = out.cells[i];
    const int rel = static_cast<int>(i) - static_cast<int>(lead);  // 0 = 1-е число
    if (rel < 0) {
      c.year = static_cast<int16_t>(py);
      c.month = static_cast<uint8_t>(pm);
      c.day = static_cast<uint8_t>(pdim + rel + 1);
      c.kind = CellKind::PrevMonth;
    } else if (static_cast<unsigned>(rel) >= dim) {
      c.year = static_cast<int16_t>(ny);
      c.month = static_cast<uint8_t>(nm);
      c.day = static_cast<uint8_t>(rel - dim + 1);
      c.kind = CellKind::NextMonth;
    } else {
      c.year = static_cast<int16_t>(year);
      c.month = static_cast<uint8_t>(month);
      c.day = static_cast<uint8_t>(rel + 1);
      c.kind = CellKind::CurrentMonth;
    }
    c.weekend = (i % kGridCols) >= 5;
  }
}

// ---- Названия --------------------------------------------------------------

namespace {

const Labels kLabelsEn = {"Wk", "Day", "Sunrise", "Sunset", "Daylight", "h", "m", "Polar day", "Polar night", "No clock"};
const Labels kLabelsRu = {"Нед.", "День", "Восход", "Закат", "Длина дня", "ч", "мин", "Полярный день", "Полярная ночь",
                          "Время не задано"};
const Labels kLabelsDe = {"KW", "Tag", "Sonnenaufg.", "Sonnenunterg.", "Tageslicht", "Std", "Min", "Polartag",
                          "Polarnacht", "Keine Uhrzeit"};

const char* const kWeekdayEn[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
const char* const kWeekdayRu[7] = {"Понедельник", "Вторник", "Среда", "Четверг", "Пятница", "Суббота", "Воскресенье"};
const char* const kWeekdayDe[7] = {"Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag", "Sonntag"};

const char* const kShortEn[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
const char* const kShortRu[7] = {"Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс"};
const char* const kShortDe[7] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};

const char* const kMonthEn[12] = {"January", "February", "March",     "April",   "May",      "June",
                                  "July",    "August",   "September", "October", "November", "December"};
const char* const kMonthRu[12] = {"Январь", "Февраль", "Март",   "Апрель", "Май",    "Июнь",
                                  "Июль",   "Август",  "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"};
const char* const kMonthDe[12] = {"Januar", "Februar", "März",      "April",   "Mai",      "Juni",
                                  "Juli",   "August",  "September", "Oktober", "November", "Dezember"};
const char* const kMonthGenRu[12] = {"января", "февраля", "марта",     "апреля",  "мая",    "июня",
                                     "июля",   "августа", "сентября", "октября", "ноября", "декабря"};

template <typename T>
const T& pick(Lang lang, const T& en, const T& ru, const T& de) {
  return lang == Lang::Ru ? ru : lang == Lang::De ? de : en;
}

}  // namespace

const Labels& labels(Lang l) { return pick(l, kLabelsEn, kLabelsRu, kLabelsDe); }

const char* weekdayName(Lang l, unsigned wd) { return pick(l, kWeekdayEn, kWeekdayRu, kWeekdayDe)[wd % 7]; }
const char* weekdayShort(Lang l, unsigned wd) { return pick(l, kShortEn, kShortRu, kShortDe)[wd % 7]; }

const char* monthName(Lang l, unsigned m) {
  if (m < 1 || m > 12) return "";
  return pick(l, kMonthEn, kMonthRu, kMonthDe)[m - 1];
}

const char* monthNameGenitive(Lang l, unsigned m) {
  if (m < 1 || m > 12) return "";
  return l == Lang::Ru ? kMonthGenRu[m - 1] : monthName(l, m);
}

int formatLongDate(Lang l, int y, unsigned m, unsigned d, char* buf, unsigned size) {
  switch (l) {
    case Lang::Ru:
      return std::snprintf(buf, size, "%u %s %d", d, monthNameGenitive(l, m), y);
    case Lang::De:
      return std::snprintf(buf, size, "%u. %s %d", d, monthName(l, m), y);
    default:
      return std::snprintf(buf, size, "%s %u, %d", monthName(l, m), d, y);
  }
}

int formatUtcOffset(int m, char* buf, unsigned size) {
  const char sign = m < 0 ? '-' : '+';
  if (m < 0) m = -m;
  if (m % 60 == 0) return std::snprintf(buf, size, "UTC%c%d", sign, m / 60);
  return std::snprintf(buf, size, "UTC%c%d:%02d", sign, m / 60, m % 60);
}

int formatShortDate(Lang l, int y, unsigned m, unsigned d, char* buf, unsigned size) {
  if (l == Lang::En) return std::snprintf(buf, size, "%04d-%02u-%02u", y, m, d);
  return std::snprintf(buf, size, "%02u.%02u.%04d", d, m, y);
}

}  // namespace calendar_core
