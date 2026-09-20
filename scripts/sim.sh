#!/usr/bin/env bash
# Симулятор (SDL2). Без аргументов — собирает и открывает окно.
#   SIM_BUILD_ONLY=1 ./scripts/sim.sh   — только собрать
#   SIM_FAKE_WIFI=1  ./scripts/sim.sh   — подложить фиктивную сохранённую сеть: симулятор «подключается» к ней,
#                                         а HTTP идёт через curl хоста — погода и город по IP работают по-настоящему
# Эмулируется именно Paper Mono (тач, 480×800): флаг -DSIMULATOR_DEVICE_PAPERMONO, отдельный каталог сборки
# work/.pio/build-sim-pm. Штатное X4-окружение upstream без тача: SIM_DEVICE=x4 ./scripts/sim.sh
# Клавиши: Esc = Back, Enter = Confirm, стрелки = Влево/Вправо/Вверх/Вниз, P = Power, S = сон, H = Домой; мышь = тач.
# Как попасть в календарь: на главной Esc (кнопка «Standby») — календарь открывается первым; стрелка Вправо — Sloppy Clock. Состояние симулятора — в work/fs_/.
# Скриншоты и ввод — CROSSPOINT_SIM_INPUT_SCRIPT / CROSSPOINT_SIM_SCREENSHOTS (см. scripts/shots.sh).
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$WORK"

# Хост-фикс для GCC >= 15 (умолчание C23): зависимость QRCode делает `typedef bool`, что в C23 запрещено.
# Правим скачанную копию в work/.pio (одноразовая), не upstream и не overlay. Идемпотентно.
if [ "${SIM_DEVICE:-papermono}" = papermono ]; then
  export PLATFORMIO_BUILD_FLAGS="${PLATFORMIO_BUILD_FLAGS:-} -DSIMULATOR_DEVICE_PAPERMONO"
  export PLATFORMIO_BUILD_DIR="$WORK/.pio/build-sim-pm"
fi
"$PIO" pkg install -e simulator >/dev/null
QR="$WORK/.pio/libdeps/simulator/QRCode/src/qrcode.h"
if [ -f "$QR" ] && ! grep -q "calmod-c23" "$QR"; then
  sed -i 's|^#ifndef __cplusplus$|#if !defined(__cplusplus) \&\& (!defined(__STDC_VERSION__) \|\| __STDC_VERSION__ < 202311L) /* calmod-c23 */|' "$QR"
fi

if [ "${SIM_FAKE_WIFI:-0}" = 1 ]; then
  mkdir -p "$WORK/fs_/.crosspoint"
  echo '{"lastConnectedSsid":"TestNet","credentials":[{"ssid":"TestNet","password":"secret"}]}' > "$WORK/fs_/.crosspoint/wifi.json"
fi

if [ "${SIM_BUILD_ONLY:-0}" = 1 ]; then
  "$PIO" run -e simulator "$@"
else
  "$PIO" run -e simulator -t run_simulator "$@"
fi
