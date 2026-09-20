#!/usr/bin/env bash
# Генерирует крупные цифры «как в интерфейсе» (Ubuntu Medium — гарнитура UI_10/UI_12) для времени и температуры.
# Во встроенных шрифтах CrossMux нет ничего крупнее 12 pt, поэтому делаем свой: только знаки 0-9 : - ° (крошечный).
#
# Размеры (pt) берутся из overlay/.../CalendarConfig.h (kTimeFontPortraitPt, kTimeFontLandscapePt, kTempFontPt).
# Метрики (высота цифры, отступ) считаются здесь же и пишутся в CalendarFontMetrics.h (namespace calendar_fonts;
# лёгкий файл, его можно включать откуда угодно; сами данные шрифта — CalendarFonts.h, включается ровно в одном месте),
# поэтому после смены размера достаточно запустить скрипт и собрать — руками ничего править не нужно.
# Забыли запустить — сборка остановится на static_assert в CalendarFace.cpp.
#
#   ./tools/gen_digit_fonts.sh
# Нужно: ../fonts-env (python3 -m venv ../fonts-env && ../fonts-env/bin/pip install freetype-py fonttools) и work/ (sync.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="${PY:-$ROOT/../fonts-env/bin/python}"
SCRIPTS="$ROOT/work/lib/EpdFont/scripts"
TTF="../builtinFonts/source/Ubuntu/Ubuntu-Medium.ttf"
DIR="$ROOT/overlay/src/activities/apps/standby"
CFG="$DIR/CalendarConfig.h"
OUT="$DIR/CalendarFonts.h"
MET="$DIR/CalendarFontMetrics.h"
CHARS_TIME='0123456789:'   # время: цифры и двоеточие
CHARS_TEMP='0123456789-°'  # температура: цифры, минус и градус

cfg() {  # cfg <имя константы> — целое из CalendarConfig.h
  local v; v="$(sed -n "s/^[[:space:]]*constexpr int $1[[:space:]]*=[[:space:]]*\([0-9][0-9]*\);.*/\1/p" "$CFG")"
  [ -n "$v" ] || { echo "нет $1 в $CFG" >&2; exit 1; }
  echo "$v"
}
PT_XL="$(cfg kTimeFontPortraitPt)"; PT_L="$(cfg kTimeFontLandscapePt)"; PT_TEMP="$(cfg kTempFontPt)"

{
  echo "// Сгенерировано tools/gen_digit_fonts.sh (Ubuntu Medium; время: $CHARS_TIME, температура: $CHARS_TEMP). Не править руками."
  echo "// Гарнитура Ubuntu — Ubuntu Font Licence 1.0 (см. work/lib/EpdFont/builtinFonts/source/Ubuntu/UFL.txt)."
  echo "#pragma once"
  cd "$SCRIPTS"
  "$PY" fontconvert.py calendar_time_xl "$PT_XL" "$TTF" --characters "$CHARS_TIME" 2>/dev/null
  "$PY" fontconvert.py calendar_time_l "$PT_L" "$TTF" --characters "$CHARS_TIME" 2>/dev/null
  "$PY" fontconvert.py calendar_temp "$PT_TEMP" "$TTF" --characters "$CHARS_TEMP" 2>/dev/null
} > "$OUT"

# Метрики из самих данных шрифта: высота цифры «0» над базовой линией и отступ от верха строки до верха цифры.
python3 - "$OUT" "$MET" "$PT_XL" "$PT_L" "$PT_TEMP" <<'PY'
import re, sys
path, met, pt_xl, pt_l, pt_t = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:6])
s = open(path, encoding="utf-8").read()

def metrics(name):
    d = re.search(r'static const EpdFontData %s = \{\s*\S+,\s*\S+,\s*\S+,\s*\d+,\s*\d+,\s*(\d+),' % name, s)
    ascender = int(d.group(1))
    g = re.search(r'%sGlyphs\[\] = \{(.*?)\};' % name, s, re.S).group(1)
    top = int(re.search(r'\{ \d+, \d+, \d+, -?\d+, (-?\d+), \d+, \d+ \}, // 0\b', g).group(1))
    return top, ascender - top

xl, l, t = metrics("calendar_time_xl"), metrics("calendar_time_l"), metrics("calendar_temp")
m = f"""// Сгенерировано tools/gen_digit_fonts.sh вместе с CalendarFonts.h. Не править руками.
#pragma once
// Метрики и размеры, с которыми сгенерирован шрифт (CalendarDraw.cpp сверяет размеры с CalendarConfig.h).
// DigitH — высота цифры «0» над базовой линией, TopOffset — от верха строки шрифта до верха цифры, px.
namespace calendar_fonts {{
constexpr int kTimePortraitPt = {pt_xl};
constexpr int kTimeLandscapePt = {pt_l};
constexpr int kTempPt = {pt_t};
constexpr int kTimeXlDigitH = {xl[0]}, kTimeXlTopOffset = {xl[1]};
constexpr int kTimeLDigitH = {l[0]}, kTimeLTopOffset = {l[1]};
constexpr int kTempDigitH = {t[0]}, kTempTopOffset = {t[1]};
}}  // namespace calendar_fonts
"""
open(met, "w", encoding="utf-8").write(m)
PY
echo "→ $OUT ($(wc -c < "$OUT") байт), $MET"
cat "$MET"
