#!/usr/bin/env bash
# Host-тесты CalendarCore/SunTimes. Arduino и SDK не нужны.
set -euo pipefail
cd "$(dirname "$0")"
SRC=../overlay/src/activities/apps/standby
mkdir -p build
g++ -std=c++17 -O1 -Wall -Wextra -I"$SRC" dump_core.cpp "$SRC/CalendarCore.cpp" -o build/dump_core
python3 verify_core.py build/dump_core
g++ -std=c++17 -O1 -Wall -Wextra -I"$SRC" test_format.cpp "$SRC/CalendarCore.cpp" -o build/test_format && ./build/test_format
g++ -std=c++17 -O1 -Wall -Wextra -I"$SRC" dump_sun.cpp "$SRC/CalendarCore.cpp" "$SRC/SunTimes.cpp" -o build/dump_sun
python3 verify_sun.py build/dump_sun

g++ -std=c++17 -O1 -Wall -Wextra test_wifi.cpp -o build/test_wifi && ./build/test_wifi
g++ -std=c++17 -O1 -Wall -Wextra test_log.cpp -o build/test_log && ./build/test_log

# HTTP-разбор и gzip: нужен uzlib из CrossMux (work/lib/uzlib — есть после ./scripts/sync.sh). Неиспользуемые функции
# uzlib (с контрольными суммами, которых в его урезанной копии нет) выбрасывает компоновщик — как в прошивке.
UZ=../work/lib/uzlib/src
if [ -f "$UZ/tinflate.c" ]; then
  gcc -O1 -c -ffunction-sections "$UZ/tinflate.c" -o build/tinflate.o
  g++ -std=c++17 -O1 -Wall -Wextra -ffunction-sections -I"$SRC" -I"$UZ" test_http.cpp "$SRC/HttpCore.cpp" build/tinflate.o \
    -Wl,--gc-sections -o build/test_http
  ./build/test_http data
else
  echo "uzlib не найден в work/lib — тест HTTP пропущен: сначала ./scripts/sync.sh"
fi

# Погода: нужен ArduinoJson (лежит в libdeps после первой сборки прошивки)
AJ=""
for d in ../work/.pio/libdeps/*/ArduinoJson/src; do [ -d "$d" ] && AJ="$d" && break; done  # из любого окружения PlatformIO
if [ -n "$AJ" ]; then
  g++ -std=c++17 -O1 -Wall -Wextra -I"$SRC" -I"$AJ" test_weather.cpp "$SRC/CalendarCore.cpp" "$SRC/WeatherCore.cpp" "$SRC/SunTimes.cpp" -o build/test_weather
  ./build/test_weather data
  g++ -std=c++17 -O1 -Wall -Wextra -I"$SRC" -I"$AJ" test_holidays.cpp "$SRC/CalendarCore.cpp" "$SRC/HolidayCore.cpp" -o build/test_holidays
  ./build/test_holidays data
else
  echo "ArduinoJson не найден в work/.pio/libdeps — тесты погоды и праздников пропущены: сначала ./scripts/build.sh или SIM_BUILD_ONLY=1 ./scripts/sim.sh"
fi

g++ -std=c++17 -O1 -Wall -Wextra -I"$SRC" dump_moon.cpp "$SRC/CalendarCore.cpp" "$SRC/MoonPhase.cpp" -o build/dump_moon
python3 verify_moon.py build/dump_moon
