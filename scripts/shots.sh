#!/usr/bin/env bash
# Скриншоты грани «Календарь» из симулятора без окна (SDL_VIDEODRIVER=dummy). Сценарии:
#   nodata      нет сети и кэша            → Москва по умолчанию, везде заглушки «Нет данных»
#   moscow      IP-геолокация не отвечает   → Москва + погода (мок Open-Meteo из tests/data)
#   live        настоящая сеть              → город по IP хоста + живая погода (нужен интернет)
#   offline     Wi-Fi недоступен после live → город и погода из кэша, пометка «обн.»
#   stale       кэш старше 3 ч              → «устарело»
#   orient      ориентации 1/2/3 из настроек (как плитка в шторке)
#   month       листание (6-строчный август) и Immersive
#   ./scripts/shots.sh [каталог] [сценарии...]     по умолчанию ./shots и все сценарии
# Требует собранный симулятор: SIM_BUILD_ONLY=1 ./scripts/sim.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"
OUT="$(mkdir -p "${1:-$ROOT/shots}" && cd "${1:-$ROOT/shots}" && pwd)"
shift || true
SCENARIOS=("$@"); [ ${#SCENARIOS[@]} -gt 0 ] || SCENARIOS=(nodata moscow live offline stale orient month)
BIN="$WORK/.pio/build/simulator/program"
[ -x "$BIN" ] || { echo "нет $BIN — SIM_BUILD_ONLY=1 ./scripts/sim.sh" >&2; exit 1; }
FS="$WORK/fs_/.crosspoint"
export SDL_VIDEODRIVER=dummy
cd "$WORK"

run() {  # run "<input script>" "<screenshots>"   (окружение — из вызывающего)
  CROSSPOINT_SIM_INPUT_SCRIPT="$1" CROSSPOINT_SIM_SCREENSHOTS="$2" timeout 120 "$BIN" 2>&1 \
    | grep -E "SIM\] Saved|\[WX\]|panic|Assert|abort|Guru" || true
}
standby() {  # standby <конец_мс> <скрины>
  run "1500:BACK;$1:QUIT" "$2"
}
set_orientation() { python3 - "$FS/settings.json" "$1" <<'PY'
import json, sys
p, v = sys.argv[1], int(sys.argv[2])
d = json.load(open(p)); d["orientation"] = v; json.dump(d, open(p, "w"))
PY
}
# Часовой пояс часов в симуляторе: Москва (UTC+3 = 48 + 12 четвертей). Переопределить: SIM_UTC_Q=48 ./scripts/shots.sh
set_utc()      { python3 - "$FS/settings.json" "${SIM_UTC_Q:-60}" <<'PY'
import json, sys
p, v = sys.argv[1], int(sys.argv[2])
d = json.load(open(p)); d["clockUtcOffsetQ"] = v; json.dump(d, open(p, "w"))
PY
}
wifi_saved()   { mkdir -p "$FS"; echo '{"lastConnectedSsid":"TestNet","credentials":[{"ssid":"TestNet","password":"secret"}]}' > "$FS/wifi.json"; }
wifi_none()    { rm -f "$FS/wifi.json"; }
cache_clear()  { rm -f "$FS/calendar_cache.json"; }
DEAD_PROXY="http://127.0.0.1:9"

# Первый запуск: экран выбора языка (список кнопочный). Русский — на 7 строк выше выделенного.
if ! grep -q '"language":"RU"' "$FS/settings.json" 2>/dev/null; then
  SC=""; t=1200; for _ in 1 2 3 4 5 6 7; do SC="$SC$t:UP;"; t=$((t+300)); done
  run "$SC$t:ENTER;$((t+2000)):QUIT" ""
fi
set_orientation 0
set_utc

for s in "${SCENARIOS[@]}"; do
  echo "== $s"
  case "$s" in
    nodata)
      wifi_none; cache_clear
      standby 9000 "8000:$OUT/wx_nodata.bmp" ;;
    moscow)
      wifi_saved; cache_clear; mkdir -p "$OUT/mock"; cp "$ROOT/tests/data/openmeteo_moscow.json" "$OUT/mock/forecast"
      CROSSPOINT_SIM_HTTP_MOCK_ROOT="$OUT/mock" https_proxy=$DEAD_PROXY HTTPS_PROXY=$DEAD_PROXY \
        standby 13000 "12000:$OUT/wx_moscow.bmp" ;;
    live)
      wifi_saved; cache_clear
      standby 20000 "19000:$OUT/wx_live.bmp" ;;
    offline)  # кэш от live остаётся; сеть «падает»
      wifi_saved
      CROSSPOINT_SIM_WIFI_CONNECT=fail standby 9000 "8000:$OUT/wx_offline_cached.bmp" ;;
    stale)    # состарим кэш на 5 часов (>3 ч, <12 ч)
      python3 - "$FS/calendar_cache.json" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p)); d["wx"]["at"] -= 5 * 3600; json.dump(d, open(p, "w"), ensure_ascii=False)
PY
      wifi_saved   # кэш просрочен → цикл стартует, но Wi-Fi «не подключается»: данные остаются с пометкой
      CROSSPOINT_SIM_WIFI_CONNECT=fail standby 9000 "8000:$OUT/wx_stale.bmp" ;;
    orient)
      for o in 1 2 3; do
        set_orientation $o
        standby 7000 "6000:$OUT/orient_$o.bmp"
      done
      set_orientation 0 ;;
    month)
      run "1500:BACK;3200:UP;7500:DOWN;8200:DOWN;12000:QUIT" \
          "2900:$OUT/month_normal.bmp;4200:$OUT/month_prev.bmp;6800:$OUT/month_immersive.bmp;9200:$OUT/month_next.bmp" ;;
    *) echo "неизвестный сценарий: $s" >&2; exit 1 ;;
  esac
done
set_orientation 0

python3 - "$OUT" <<'PY'
import sys, glob
from PIL import Image
for p in sorted(glob.glob(sys.argv[1] + "/*.bmp")):
    Image.open(p).save(p[:-4] + ".png")
    print("→", p[:-4] + ".png")
PY
