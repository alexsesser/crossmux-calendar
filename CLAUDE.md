# CrossMux Calendar Face

@CONCEPT.md

## Прочие файлы проекта
- `README.md` — что это, как собирать/прошивать/обновляться, как править параметры (пользовательская документация; держать в актуальном состоянии)
- `hooks/apply_hooks.py` — идемпотентные правки upstream (см. концепцию, §5.1)
- `design/calendar-interactive-mockup.html` — кликабельный прототип тапа по погоде и календарю (концепция — CONCEPT.md §12)
- `overlay/` — наш код, копируется в `work/` как есть: `CalendarFace` (главный экран, состояние, ввод), `CalendarDetail` (экраны «Погода/День/Год», карта тап-зон), `CalendarDraw` (общая отрисовка), `HolidayCore` (праздники и переносы), `MoonPhase`, `CalendarCore` (логика, host-тесты), `SunTimes`,
  `WeatherCore` (разбор JSON, коды WMO, кэш — host-тесты), `WeatherClient` (Wi-Fi + HTTP, из `tick()` грани),
  `CalendarOrientation` (хук 3), `CalendarConfig` (ЕДИНСТВЕННЫЙ файл настроек: интервалы, город по умолчанию, раскладка, размеры шрифта, порядок граней — новые настраиваемые числа добавлять только сюда, не в другие файлы), `CalendarFonts.h` (СГЕНЕРИРОВАН tools/gen_digit_fonts.sh — руками не править)
- `scripts/` — `sync.sh` (клон+overlay+хуки), `overlay.sh` (быстро накатить overlay на готовый `work/`), `build.sh`,
  `sim.sh`, `shots.sh` (скриншоты из симулятора без окна), `flash.sh`,
  `update.sh` (проверка нового upstream; пишет upstream.lock только при зелёной сборке)
- `tests/run.sh` — host-тесты CalendarCore/SunTimes (нужен g++ и python3; для сверки солнца — `astral`)
- Шрифты: см. CONCEPT §4.6 — во встроенных `NOTOSANS_*` кириллицы нет, текст только `UI_10/UI_12`
- `tools/gen_digit_fonts.sh` — генератор шрифтов цифр (время/температура — Ubuntu Medium, числа сетки — Regular/Medium по `kGridDigitsBold`); `docs/` — скриншоты для README
