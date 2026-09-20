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

# Погода: нужен ArduinoJson (лежит в libdeps после первой сборки прошивки)
AJ=../work/.pio/libdeps/papermono/ArduinoJson/src
if [ -d "$AJ" ]; then
  g++ -std=c++17 -O1 -Wall -Wextra -I"$SRC" -I"$AJ" test_weather.cpp "$SRC/CalendarCore.cpp" "$SRC/WeatherCore.cpp" -o build/test_weather
  ./build/test_weather data
  g++ -std=c++17 -O1 -Wall -Wextra -I"$SRC" -I"$AJ" test_holidays.cpp "$SRC/CalendarCore.cpp" "$SRC/HolidayCore.cpp" -o build/test_holidays
  ./build/test_holidays data
else
  echo "ArduinoJson не найден ($AJ) — тесты погоды пропущены: сначала ./scripts/build.sh"
fi

g++ -std=c++17 -O1 -Wall -Wextra -I"$SRC" dump_moon.cpp "$SRC/CalendarCore.cpp" "$SRC/MoonPhase.cpp" -o build/dump_moon
python3 verify_moon.py build/dump_moon
