// Форматирование часового пояса (CalendarCore::formatUtcOffset).
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "CalendarCore.h"

int main() {
  struct C { int min; const char* want; };
  int fails = 0;
  for (C c : {C{180, "UTC+3"}, C{0, "UTC+0"}, C{330, "UTC+5:30"}, C{345, "UTC+5:45"}, C{-210, "UTC-3:30"}, C{-720, "UTC-12"},
              C{840, "UTC+14"}, C{-30, "UTC-0:30"}, C{60, "UTC+1"}, C{120, "UTC+2"}}) {
    char b[16];
    calendar_core::formatUtcOffset(c.min, b, sizeof(b));
    if (std::strcmp(b, c.want) != 0) { std::printf("FAIL %d -> '%s', ожидалось '%s'\n", c.min, b, c.want); ++fails; }
  }
  // civilFromDays — обратная к daysFromCivil на всём диапазоне
  for (int32_t z = -3000; z < 60000; ++z) {
    int y; unsigned m, d; calendar_core::civilFromDays(z, y, m, d);
    if (calendar_core::daysFromCivil(y, m, d) != z || m < 1 || m > 12 || d < 1 || d > calendar_core::daysInMonth(y, m)) { std::printf("FAIL civil %d\n", z); ++fails; break; }
  }
  std::printf("format: ошибок %d\n", fails);
  return fails;
}
