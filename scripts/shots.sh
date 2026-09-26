#!/usr/bin/env bash
# Скриншоты грани «Календарь» из симулятора без окна (SDL_VIDEODRIVER=dummy). Сценарии:
#   nodata      нет сети и кэша            → Москва по умолчанию, везде заглушки «Нет данных»
#   moscow      IP-геолокация не отвечает   → Москва + погода (мок Open-Meteo из tests/data)
#   live        настоящая сеть              → город по IP хоста + живая погода (нужен интернет)
#   offline     Wi-Fi недоступен после live → город и погода из кэша, пометка «обн.»
#   stale       кэш старше 3 ч              → «устарело»
#   orient      ориентации 1/2/3 из настроек (как плитка в шторке)
#   month       листание (6-строчный август) и Immersive
#   docs        кадры для README: Москва (мок геолокации и прогноза) + живой календарь праздников
#   docs_place  кадр экрана «Место» для README (подложенные данные, поиск города — мок)
#   hang        сеть «зависла» (прокси в никуда): экран и тапы должны работать, пока сетевая задача ждёт
#   fallback    Open-Meteo недоступен ни по HTTPS, ни по HTTP → погода от MET Norway (мок: настоящий gzip-ответ на 26.09.2026)
#   holidays_offline нет сети и календаря: май 2026 и 2027 — только выходные и фиксированные праздники
#   detail_land то же в ландшафте (погода ×2, день, год)
#   detail      тапы: погода (сегодня, 7 дней), день, май с праздниками, 9 мая, год (нужен интернет, эмулируется Paper Mono с тачем)
#   ./scripts/shots.sh [каталог] [сценарии...]     по умолчанию ./shots и все сценарии
# Требует собранный симулятор: SIM_BUILD_ONLY=1 ./scripts/sim.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"
OUT="$(mkdir -p "${1:-$ROOT/shots}" && cd "${1:-$ROOT/shots}" && pwd)"
shift || true
SCENARIOS=("$@"); [ ${#SCENARIOS[@]} -gt 0 ] || SCENARIOS=(nodata moscow live offline stale orient month detail detail_land holidays_offline hang)  # docs — отдельно: ./scripts/shots.sh shots docs
BIN="$SIM_BIN"
[ -x "$BIN" ] || { echo "нет $BIN — SIM_BUILD_ONLY=1 ./scripts/sim.sh" >&2; exit 1; }
FS="$WORK/fs_/.crosspoint"
export SDL_VIDEODRIVER=dummy
cd "$WORK"

run() {  # run "<input script>" "<screenshots>"   (окружение — из вызывающего)
  CROSSPOINT_SIM_INPUT_SCRIPT="$1" CROSSPOINT_SIM_SCREENSHOTS="$2" timeout 120 "$BIN" 2>&1 \
    | grep -E "SIM\] Saved|\[WX\]|\[HTTP\]|panic|Assert|abort|Guru" || true
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
cache_clear()  { rm -f "$FS/calendar_cache.json" "$FS/calendar_holidays.json"; }
DEAD_PROXY="http://127.0.0.1:9"

# Язык — русский, знакомство пройдено: пишем настройки напрямую (выбор языка кнопками в списке зависит от того,
# с какого элемента он стартует, и ненадёжен). Остальные поля SETTINGS берёт по умолчанию.
mkdir -p "$FS"
python3 - "$FS/settings.json" <<'PY'
import json, os, sys
p = sys.argv[1]
d = json.load(open(p)) if os.path.exists(p) else {}
d.update({"language": "RU", "onboardingVersion": d.get("onboardingVersion", 1), "langSku": "global"})
json.dump(d, open(p, "w"))
PY
set_orientation 0
set_utc

for s in "${SCENARIOS[@]}"; do
  echo "== $s"
  case "$s" in
    nodata)
      wifi_none; cache_clear
      standby 9000 "8000:$OUT/wx_nodata.bmp" ;;
    moscow)
      wifi_saved; cache_clear; mkdir -p "$OUT/mock"; cp "$ROOT/tests/data/openmeteo_moscow_7d.json" "$OUT/mock/forecast"
      CROSSPOINT_SIM_HTTP_MOCK_ROOT="$OUT/mock" https_proxy=$DEAD_PROXY HTTPS_PROXY=$DEAD_PROXY \
        standby 13000 "12000:$OUT/wx_moscow.bmp" ;;
    fallback)
      wifi_saved; cache_clear; mkdir -p "$OUT/mock_fb"; rm -f "$OUT/mock_fb"/*
      cp "$ROOT/tests/data/metno_moscow_complete.json.gz" "$OUT/mock_fb/complete"
      CROSSPOINT_SIM_HTTP_MOCK_ROOT="$OUT/mock_fb" https_proxy=$DEAD_PROXY HTTPS_PROXY=$DEAD_PROXY \
        http_proxy=$DEAD_PROXY HTTP_PROXY=$DEAD_PROXY \
        run "1500:BACK;16000:TAP:240,330;19000:SWIPE:400,400,100,400;22000:QUIT" \
          "15000:$OUT/wx_metno_main.bmp;18500:$OUT/wx_metno_weather.bmp;21500:$OUT/wx_metno_7d.bmp" ;;
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
    detail)
      # Координаты тапов — для портрета 480×800 с погодой: блок погоды ≈ y 260–450, сетка сентября: строка «21…27» y≈690,
      # название месяца y≈487, «9 мая» в мае (сб, вторая строка) ≈ (360, 604).
      # До первого тапа — пауза ≈ 12 с: сеть стартует только после тишины (kOnDemandDebounceMs) и загружает данные.
      wifi_saved; cache_clear
      run "1500:BACK;13000:TAP:240,330;15500:SWIPE:400,400,100,400;18000:BACK;20000:TAP:60,690;22000:SWIPE:400,400,100,400;24000:BACK;24500:UP;25000:UP;25500:UP;26000:UP;28000:TAP:360,604;30000:BACK;30500:TAP:240,487;32500:SWIPE:400,400,100,400;35000:QUIT" \
          "15000:$OUT/detail_1_weather_today.bmp;17500:$OUT/detail_2_weather_week.bmp;19500:$OUT/detail_3_main.bmp;21500:$OUT/detail_4_day.bmp;23500:$OUT/detail_5_day_next.bmp;27500:$OUT/detail_6_may.bmp;29500:$OUT/detail_7_day_holiday.bmp;32000:$OUT/detail_8_year.bmp;34000:$OUT/detail_9_year_next.bmp" ;;
    detail_land)
      # В ландшафте симулятор трактует целые координаты как портретные 480×800 — берём нормализованные (0..1):
      # блок погоды (150,340), «21 сентября» (388,335), название месяца (560,55) из 800×480.
      wifi_saved; set_orientation 3   # кэш от detail остаётся
      run "1500:BACK;13000:TAP:0.19,0.71;15500:SWIPE:0.75,0.62,0.25,0.62;18000:BACK;20500:TAP:0.485,0.70;23000:BACK;25000:TAP:0.70,0.115;28000:QUIT" \
          "15000:$OUT/land_1_weather_today.bmp;17500:$OUT/land_2_weather_week.bmp;22000:$OUT/land_3_day.bmp;27000:$OUT/land_4_year.bmp"
      set_orientation 0 ;;
    docs)
      wifi_saved; cache_clear; mkdir -p "$OUT/mock"
      cp "$ROOT/tests/data/openmeteo_moscow_7d.json" "$OUT/mock/forecast"
      # Место — Москва «по IP, определено только что» (мок ipwhois для URL вида /json/?… не подходит по имени файла).
      python3 - "$FS/calendar_cache.json" <<'PY'
import json, sys, time
json.dump({"v": 2, "place": {"lat": 55.7558, "lon": 37.6173, "city": "Москва", "ip": 1, "ipAt": int(time.time()), "ipLang": 1}}, open(sys.argv[1], "w"), ensure_ascii=False)
PY
      # (Первое из пяти «вверх» лишь будит Immersive.) Первые ≈ 20 с — тишина: идут два сетевых цикла (погода + первые 3 года календаря, затем ещё 3 года предзагрузки).
      CROSSPOINT_SIM_HTTP_MOCK_ROOT="$OUT/mock" run "1500:BACK;22000:TAP:240,330;24500:SWIPE:400,400,100,400;27000:BACK;33000:UP;33500:UP;34000:UP;34500:UP;35000:UP;36500:TAP:360,604;38500:BACK;39000:TAP:240,487;41500:QUIT" \
          "21000:$OUT/docs_1_main.bmp;24000:$OUT/docs_2_weather_today.bmp;26500:$OUT/docs_3_weather_week.bmp;36000:$OUT/docs_4_may.bmp;38000:$OUT/docs_5_day.bmp;41000:$OUT/docs_6_year.bmp" ;;
    docs_place)
      # Экран «Место» для README: по IP — «чужой» город через офисную сеть (данные подложены в кэш, настоящий IP не нужен),
      # вручную — Москва; поиск «москва» с экранной клавиатуры (ответ Open-Meteo Geocoding — из tests/data, мок).
      wifi_saved; cache_clear; mkdir -p "$OUT/mock"
      cp "$ROOT/tests/data/openmeteo_moscow_7d.json" "$OUT/mock/forecast"
      cp "$ROOT/tests/data/geocode_moskva_ru.json" "$OUT/mock/search"
      python3 - "$FS/calendar_cache.json" "$FS/calendar_settings.json" <<'PY'
import json, sys, time
json.dump({"v": 2, "place": {"lat": 50.1109, "lon": 8.6821, "city": "Франкфурт-на-Майне", "ip": 1, "ipAt": int(time.time()) - 600,
           "ipLang": 1, "ssid": "Office-WiFi"}}, open(sys.argv[1], "w"), ensure_ascii=False)
json.dump({"v": 1, "auto": False, "log": True, "manual": {"city": "Москва", "lat": 55.7558, "lon": 37.6173}},
          open(sys.argv[2], "w"), ensure_ascii=False)
PY
      # Тапы клавиатуры (русская раскладка, портрет): м о с к в а, затем OK.
      CROSSPOINT_SIM_HTTP_MOCK_ROOT="$OUT/mock" run "1500:BACK;9000:TAP:240,330;11500:TAP:275,57;14000:TAP:122,410;16500:TAP:200,720;17300:TAP:280,675;18100:TAP:162,720;18900:TAP:143,627;19700:TAP:118,675;20500:TAP:158,675;21500:TAP:418,770;29000:QUIT" \
          "28500:$OUT/docs_7_place.bmp"
      rm -f "$FS/calendar_settings.json" ;;
    docs_land)
      wifi_saved; cache_clear; mkdir -p "$OUT/mock"; set_orientation 3
      cp "$ROOT/tests/data/openmeteo_moscow_7d.json" "$OUT/mock/forecast"
      python3 - "$FS/calendar_cache.json" <<'PY'
import json, sys, time
json.dump({"v": 2, "place": {"lat": 55.7558, "lon": 37.6173, "city": "Москва", "ip": 1, "ipAt": int(time.time()), "ipLang": 1}}, open(sys.argv[1], "w"), ensure_ascii=False)
PY
      CROSSPOINT_SIM_HTTP_MOCK_ROOT="$OUT/mock" run "1500:BACK;23000:QUIT" "22000:$OUT/docs_land_main.bmp"
      set_orientation 0 ;;
    hang)
      wifi_saved; cache_clear
      https_proxy=http://10.255.255.1:3128 HTTPS_PROXY=http://10.255.255.1:3128 \
        run "1500:BACK;9000:TAP:240,330;12000:BACK;14000:QUIT" \
            "6500:$OUT/hang_1_main.bmp;10500:$OUT/hang_2_weather_opened.bmp;13500:$OUT/hang_3_closed.bmp" ;;
    holidays_offline)
      wifi_none; cache_clear
      run "1500:BACK;3000:UP;3500:UP;4000:UP;4500:UP;7500:QUIT" "6500:$OUT/holidays_offline_may.bmp" ;;
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
