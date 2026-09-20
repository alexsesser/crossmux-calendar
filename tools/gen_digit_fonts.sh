#!/usr/bin/env bash
# Генерирует крупные цифры «как в интерфейсе» (Ubuntu Medium — гарнитура UI_10/UI_12) для времени и температуры.
# Во встроенных шрифтах CrossMux нет ничего крупнее 12 pt, поэтому делаем свой: только знаки 0-9 : - ° (крошечный).
# Результат — overlay/.../CalendarFonts.h (в git). Запускать нужно ТОЛЬКО при смене размеров/гарнитуры.
#   ./tools/gen_digit_fonts.sh
# Нужно: ../fonts-env (python3 -m venv ../fonts-env && ../fonts-env/bin/pip install freetype-py fonttools) и work/ (sync.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="${PY:-$ROOT/../fonts-env/bin/python}"
SCRIPTS="$ROOT/work/lib/EpdFont/scripts"
TTF="../builtinFonts/source/Ubuntu/Ubuntu-Medium.ttf"
OUT="$ROOT/overlay/src/activities/apps/standby/CalendarFonts.h"
CHARS='0123456789:-°'

# pt @150 dpi (ppem = pt*150/72). Высота цифры ≈ 0.69 ppem.
SIZE_TIME_XL="${SIZE_TIME_XL:-70}"   # время, портрет  (цифра ≈ 105 px)
SIZE_TIME_L="${SIZE_TIME_L:-50}"     # время, ландшафт (цифра ≈ 76 px)
SIZE_TEMP="${SIZE_TEMP:-25}"         # температура     (цифра ≈ 36 px)

{
  echo "// Сгенерировано tools/gen_digit_fonts.sh (Ubuntu Medium, знаки: $CHARS). Не править руками."
  echo "// Гарнитура Ubuntu — Ubuntu Font Licence 1.0 (см. work/lib/EpdFont/builtinFonts/source/Ubuntu/UFL.txt)."
  echo "#pragma once"
  cd "$SCRIPTS"
  for spec in "calendar_time_xl:$SIZE_TIME_XL" "calendar_time_l:$SIZE_TIME_L" "calendar_temp:$SIZE_TEMP"; do
    "$PY" fontconvert.py "${spec%%:*}" "${spec##*:}" "$TTF" --characters "$CHARS"
  done
} > "$OUT"
echo "→ $OUT ($(wc -c < "$OUT") байт)"
