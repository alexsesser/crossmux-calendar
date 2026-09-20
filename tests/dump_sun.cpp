#include <cstdio>
#include <initializer_list>

#include "CalendarCore.h"
#include "SunTimes.h"

struct City { const char* name; double lat, lon; int off; };

int main() {
  const City cities[] = {
      {"Moscow", 55.7558, 37.6173, 180},   {"Kaliningrad", 54.7104, 20.4522, 120}, {"Berlin", 52.52, 13.405, 60},
      {"Kyiv", 50.4501, 30.5234, 120},     {"Sydney", -33.8688, 151.2093, 600},   {"NewYork", 40.7128, -74.006, -300},
      {"Reykjavik", 64.1466, -21.9426, 0}, {"Quito", -0.1807, -78.4678, -300},    {"Tromso", 69.6492, 18.9553, 60},
      {"Vladivostok", 43.1155, 131.8855, 600}, {"Auckland", -36.85, 174.76, 720},
  };
  for (const City& c : cities) {
    for (int m = 1; m <= 12; ++m) {
      for (unsigned d : {1u, 10u, 20u}) {
        const sun_times::Result r = sun_times::compute(2026, m, d, c.lat, c.lon, c.off);
        std::printf("%s %.4f %.4f %d 2026 %d %u %d %d %d %d %d\n", c.name, c.lat, c.lon, c.off, m, d, r.valid,
                    r.polarDay, r.polarNight, r.sunriseMin, r.sunsetMin);
      }
    }
  }
}
