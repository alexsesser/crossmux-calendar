#include <cstdio>
#include "MoonPhase.h"
int main() {
  // k от 1970 (≈ -360) до 2100 (≈ +1240)
  for (int k = -400; k <= 1250; ++k) {
    std::printf("N %d %.6f\n", k, moon_phase::eventJdUt(k, false));
    std::printf("F %d %.6f\n", k, moon_phase::eventJdUt(k, true));
  }
  // Фаза на момент: 2026-09-20 12:00 МСК
  const double jd = moon_phase::julianFromLocal(2026, 9, 20, 12, 0, 180);
  auto i = moon_phase::at(jd);
  auto nf = moon_phase::nextFullMoon(jd, 180); auto nn = moon_phase::nextNewMoon(jd, 180);
  std::printf("P %.4f %.4f %d %d | nextFull %d-%02u-%02u nextNew %d-%02u-%02u\n", i.fraction, i.illum, i.lunarDay, i.phaseIdx, nf.year, nf.month, nf.day, nn.year, nn.month, nn.day);
}
