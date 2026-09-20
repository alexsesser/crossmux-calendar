# CrossMux Calendar Face

@CONCEPT.md

## Прочие файлы проекта
- `README.md` — что это, как собирать/прошивать/обновляться, как править параметры (пользовательская документация; держать в актуальном состоянии)
- `hooks/apply_hooks.py` — идемпотентные правки upstream (см. концепцию, §5.1)
- `design/calendar-standby-mockup.html` — макет экрана, открывать в браузере
- `overlay/` — наш код, копируется в `work/` как есть: `CalendarFace` (экран), `CalendarCore` (логика, host-тесты), `SunTimes`,
  `WeatherCore` (разбор JSON, коды WMO, кэш — host-тесты), `WeatherClient` (Wi-Fi + HTTP, из `tick()` грани),
  `CalendarOrientation` (хук 3), `CalendarConfig` (интервалы), `CalendarFonts.h` (СГЕНЕРИРОВАН tools/gen_digit_fonts.sh — руками не править)
- `scripts/` — `sync.sh` (клон+overlay+хуки), `overlay.sh` (быстро накатить overlay на готовый `work/`), `build.sh`,
  `sim.sh`, `shots.sh` (скриншоты из симулятора без окна), `backup.sh` (дамп флеша), `flash.sh`,
  `update.sh` (проверка нового upstream; пишет upstream.lock только при зелёной сборке)
- `tests/run.sh` — host-тесты CalendarCore/SunTimes (нужен g++ и python3; для сверки солнца — `astral`)
- Шрифты: см. CONCEPT §4.6 — во встроенных `NOTOSANS_*` кириллицы нет, текст только `UI_10/UI_12`
- `tools/gen_digit_fonts.sh` — генератор шрифта крупных цифр (Ubuntu Medium); `docs/` — скриншоты для README
