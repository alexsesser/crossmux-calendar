// Выгружает CalendarCore для каждой даты 1970..2100 — сверяется python-ом (verify_core.py).
#include <cstdio>

#include "CalendarCore.h"
using namespace calendar_core;

int main() {
  for (int y = kMinYear; y <= kMaxYear; ++y) {
    for (unsigned m = 1; m <= 12; ++m) {
      MonthGrid g;
      buildMonthGrid(y, m, g);
      std::printf("G %d %u %u", y, m, g.rows);
      for (unsigned i = 0; i < g.rows * 7u; ++i) {
        const Cell& c = g.cells[i];
        std::printf(" %d-%u-%u:%d:%d", c.year, c.month, c.day, static_cast<int>(c.kind), c.weekend ? 1 : 0);
      }
      std::printf("\n");
      for (unsigned d = 1; d <= daysInMonth(y, m); ++d) {
        int iy = 0;
        const unsigned w = isoWeek(y, m, d, &iy);
        std::printf("D %d %u %u %u %u %d %u %u\n", y, m, d, weekday(y, m, d), dayOfYear(y, m, d), iy, w,
                    daysInYear(y));
      }
    }
  }
  // Навигация по месяцам: границы. (Результат читаем после вызова — порядок аргументов printf не определён.)
  auto nav = [](const char* tag, int y, unsigned m, int delta) {
    int oy = 0;
    unsigned om = 0;
    const bool ok = addMonths(y, m, delta, oy, om);
    std::printf("%s %d %d %u\n", tag, ok ? 1 : 0, oy, om);
  };
  nav("N1", 1970, 1, -1);
  nav("N2", 2100, 12, 1);
  nav("N3", 2026, 9, -9);
  nav("N4", 2026, 1, -1);
  nav("N5", 2026, 12, 1);
  nav("N6", 2026, 9, 27);
  return 0;
}
