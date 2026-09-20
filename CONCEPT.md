# CrossMux Calendar Face — концепция и план разработки

Свой экран ожидания «Календарь» для **M5Stack Paper Mono** вместо китайского
календаря (老黄历), в виде **overlay + хуки** поверх любого состояния upstream.

| | |
|---|---|
| Upstream | `https://github.com/0x1abin/crossmux`, ветка `main` |
| Проверено на коммите | `c92edca422bc2a3087e22c2d47956e562ae1006e` (2026-09-20, версия 1.6.0) |
| Цель сборки | `pio run -e papermono` |
| Хост разработки | Arch Linux |
| Макет | `design/calendar-standby-mockup.html` (открыть в браузере) |

> Все ссылки `файл:строка` ниже — по коммиту `c92edca`. При обновлении upstream
> они могут сместиться; ищите по имени символа, а не по номеру строки.

---

## 1. Что делаем

**Задача.** Заменить грань стендбая «китайский календарь» на обычный григорианский
календарь: крупное время, день недели, дата в двух форматах, сетка месяца,
номер недели и день года, погода с восходом/закатом. Работает в портрете и
в ландшафте.

**Главное ограничение.** Не форкать upstream целиком. Свой репозиторий не содержит
кода CrossMux вообще: только новые файлы (overlay), скрипт точечных правок (hooks)
и сборочные скрипты. Обновление upstream = пересоздать чистый клон и применить
overlay заново. Конфликтов git не бывает by design; вместо них — громкая ошибка
`ANCHOR LOST`, если upstream изменил форму якоря.

**Чего не делаем.** Не удаляем `ChineseCalendarFace.*` и `ChineseAlmanac.*` из
исходников upstream — только не регистрируем их в таблице граней. Меньше правок =
меньше поверхность для дрейфа.

---

## 2. Железо и его следствия

M5Stack Paper Mono (SKU C153), отдельная ESP32-S3 цель в CrossMux:

- ESP32-S3R8, 16 MB flash, **8 MB PSRAM** (в отличие от C3-целей X3/X4 — там ~380 KB RAM и никакого PSRAM)
- Экран 3.97″, **480×800**, SSD1677, 4 градации серого, встроенная подсветка
- Тач FT6336G (активная зона сужена: X 5–475, Y 5–795)
- RTC **RX8130CE** — часы переживают потерю питания
- IMU BMI270, microSD, кнопки A/B + Power, Wi-Fi/BLE

Следствия для нашей грани:

1. **Часы идут без сети.** Wi-Fi нужен только для погоды, не для времени.
2. **Память не узкое место на этой плате**, но грань живёт в общем дереве исходников
   с C3-целями. Правила upstream (см. §6) соблюдаем в любом случае.
3. **Grayscale есть** — можно использовать 4 уровня для выходных и дней соседних месяцев.
4. **Автоповорот по IMU** — потенциально возможен, но сначала надо проверить, отдаёт ли
   HAL данные BMI270 для papermono. В v1 ориентацию берём из настройки CrossMux.

---

## 3. Дизайн

Макет: `design/calendar-standby-mockup.html` — в нём переключатель
портрет/ландшафт и инверсия.

**Состав экрана (сверху вниз, портрет):**

| Блок | Содержимое |
|---|---|
| Статус-строка | «синхр. HH:MM · UTC+N» слева, батарея справа |
| Время | `HH:MM`, очень крупно |
| День недели | «Воскресенье» |
| Дата | `20 сентября 2026` **и** плашка `20.09.2026` — одновременно |
| Чипсы | «Нед. 38» · «День 263 / 365» |
| Погода | иконка, температура, описание, «ощущ.»; ↓min ↑max, ветер, осадки; восход / закат / длина дня |
| Сетка месяца | заголовок «Сентябрь 2026» со стрелками ‹ ›, 7 колонок Пн–Вс, 5 строк |

**Ландшафт:** две колонки. Слева (≈330 px) всё, кроме сетки; справа — сетка месяца.
В ландшафте у блока погоды **нет нижней разделительной линии**, а подпись «день»
у длины дня скрыта, чтобы строка не переносилась.

**Оттенки серого:**
- чёрный — время и сегодняшний день (инверсная плашка);
- тёмно-серый — выходные;
- светло-серый — дни соседних месяцев;
- если grayscale недоступен — различаем жирностью и рамкой, не цветом.

**Колонка номеров недель в сетке — убрана намеренно.** Номер недели живёт только
в чипсах под временем.

---

## 4. Как устроен стендбай в upstream (результат разведки)

Каталог: `src/activities/apps/standby/` (2183 строки суммарно).

```
StandbyActivity.{h,cpp}   633+67  — активность: жесты, Wi-Fi/NTP, light sleep, grayscale-проход
StandbyFace.h                 77  — базовый интерфейс грани
SloppyClockFace.{h,cpp}     79+34  — грань «рисованные часы» (всегда доступна)
ChineseCalendarFace.{h,cpp} 409+51 — грань 老黄历 (только портрет) ← её заменяем
ChineseAlmanac.{h,cpp}     691+65  — лунный календарь, ганьчжи, солнечные термины
StandbyTime.{h,cpp}         53+24  — общие хелперы времени
```

### 4.1. Интерфейс грани — `StandbyFace.h`

```cpp
class StandbyFace {
 public:
  enum class TickResult : uint8_t { None, Redraw, RedrawWithGhostCleanup };

  virtual void onEnter() {}                 // выделить состояние
  virtual void onExit()  {}                 // освободить всё, что выделено в onEnter
  virtual void onShake(uint32_t seed) {}    // «встряска» (боковые Up/Down)
  virtual void onPagePrev() {}              // ← Up
  virtual void onPageNext() {}              // ← Down
  virtual TickResult tick() = 0;            // вызывается каждый loop(); решает, нужна ли перерисовка
  virtual void render(GfxRenderer&, const Rect& viewport) = 0;
  virtual StrId titleId() const = 0;        // заголовок в шапке
  virtual uint32_t secondsUntilNextWake() const = 0;  // для light sleep
  virtual bool wantsGrayscale() const { return false; }
};
```

Важно про `wantsGrayscale()`: при включённом grayscale `render()` вызывается
**три раза подряд** (BW, LSB, MSB). Значит `render()` обязан быть идемпотентным —
никакой мутации состояния внутри. Grayscale-проход срабатывает только в режиме
Immersive и при выключенной инверсии.

### 4.2. Регистрация граней — `StandbyActivity.cpp:54`

```cpp
constexpr FaceEntry kFaces[] = {
    {[]() -> std::unique_ptr<StandbyFace> { return makeUniqueNoThrow<SloppyClockFace>(); },
     [](int, int) { return true; }},
#ifdef ENABLE_CHINESE_VERSION
    {[]() -> std::unique_ptr<StandbyFace> { return makeUniqueNoThrow<ChineseCalendarFace>(); },
     [](int sw, int sh) { return sh > sw; }},  // portrait only
#endif
};
```

Второй лямбда-параметр — предикат доступности `isAvailable(sw, sh)`. Через него
грань скрывается в неподходящей ориентации. Наша грань возвращает `true` всегда.

`ENABLE_CHINESE_VERSION=1` задан в профиле `[base]` (`platformio.ini:69`), то есть
китайская грань собирается и в `papermono`. Это и есть то, что мы убираем.

### 4.3. Ввод — что достаётся грани, а что нет

`StandbyActivity::loop()` (`StandbyActivity.cpp:428…`) разбирает ввод **до** грани:

| Жест / кнопка | Кто обрабатывает | Действие |
|---|---|---|
| свайп ←/→ (`:434`) | активность | переключение граней |
| кнопки Left/Right (`:494`) | активность | переключение граней |
| свайп ↑/↓ | грань | `onPageNext()` / `onPagePrev()` |
| кнопки Up/Down (`:474`) | грань | `onPagePrev()` / `onPageNext()` |
| тап по экрану (`:459`) | активность | переключение инверсии; **координаты грани не передаются** |
| Confirm (`:520`) | активность | инверсия |
| Back | активность | выход на Home |

**Следствия для дизайна:**
- Листать месяц можно только через Up/Down (кнопки A/B или свайп ↑/↓).
- Стрелки `‹ ›` в макете — **декоративные**: тапа по ним сейчас не будет.
  Либо убрать их, либо оставить как подсказку для кнопок.
- Возврат к текущему месяцу делаем по таймеру бездействия, а не тапом по заголовку.
- Тап по блоку погоды (бэклог) потребует нового метода `onTap(x, y)` в `StandbyFace`
  и ещё одного хука в `loop()`. Это третий хук — добавляем только когда всё остальное стабильно.

### 4.4. Время

`src/util/TimeUtils.h` — всё, что нужно, уже есть:

```cpp
bool     TimeUtils::isClockValid();
uint32_t TimeUtils::getCurrentValidTimestamp();          // 0, если часам нельзя верить
bool     TimeUtils::getLocalDateTime(uint32_t, std::tm&); // с учётом clockUtcOffsetQ
unsigned TimeUtils::getDaysInMonth(int year, unsigned month);
uint32_t TimeUtils::getDayOrdinalForDate(int, unsigned, unsigned);
bool     TimeUtils::getDateFromDayOrdinal(uint32_t, int&, unsigned&, unsigned&);
```

CrossMux **не вызывает** `setenv("TZ")`: местное время = системное UTC + фиксированное
смещение `SETTINGS.clockUtcOffsetQ` (в четвертях часа). Не менять TZ глобально —
это сломает остальные экраны.

Так же устроена навигация по дням в `ChineseCalendarFace.cpp:281…301` — хороший
образец для нашей логики «сегодня + смещение».

### 4.5. Wi-Fi в стендбае

`StandbyActivity::startTimeSync()` выходит сразу:

```cpp
if (TimeUtils::isClockValid() || !SETTINGS.clockAutoSync) return;
```

То есть **при живых часах Wi-Fi не включается вообще**, а после синхронизации
`stopTimeSyncWifi()` делает `esp_wifi_deinit()`. У Paper Mono есть RTC, значит
часы почти всегда валидны → для погоды нужен **свой цикл выхода в сеть**
(раз в 30–60 мин).

**Реализовано без хука в `loop()`**: цикл — конечный автомат `WeatherClient`, который крутится из
`CalendarFace::tick()` (активность вызывает его каждый `loop()`). Он не мешает синхронизации времени:
Wi-Fi поднимается, только если он свободен (`WiFi.getMode() == WIFI_MODE_NULL`); если кто-то уже
подключён — используем и не выключаем. Сохранённая сеть берётся так же, как в `trySilentWifiConnect()`
(`WIFI_STORE.getLastConnectedSsid()`). HTTP — `HttpDownloader::fetchUrl` (TLS с бандлом сертификатов).

### 4.6. Шрифты и крупные цифры

`src/fontIds.h` объявляет NotoSans/NotoSerif 12/14/16/18 pt, UI 10/12, SMALL — **но это обман для
этой (унифицированной) сборки.** В `src/main.cpp:395-404` все `NOTOSANS_*` и `NOTOSERIF_*` регистрируются
на **один** `offlineReaderFontFamily` = `notosans_cjk_12`: ASCII + CJK, **без кириллицы, без жирного,
без разных размеров** (размеры даёт только загрузка шрифтов с SD). Кириллица в них рисуется как «ничто»
(глиф не найден → тихий пропуск), цифры при этом видны — поэтому ошибка легко пропускается.
Проверено в симуляторе (§8, этап 3).

Рабочие международные шрифты: **`UI_12_FONT_ID`** (Ubuntu Medium + Bold), **`UI_10_FONT_ID`**
(то же, мельче), `SMALL_FONT_ID` (notosans_8). **Больше 12 pt текста нет.**

Значит:
1. Любой текст, где есть не-ASCII (русский, немецкий), — только `UI_10/UI_12` (жирный — `EpdFontFamily::BOLD`).
2. Крупное время и температура — **свой сгенерированный шрифт** `CalendarFonts.h` (Ubuntu Medium — та же
   гарнитура, что у `UI_*`; знаки `0-9 : - °`; три размера; `tools/gen_digit_fonts.sh`, ~12 КБ в образе).
   Регистрируется в рендерере из `CalendarFace::render()` через `insertFont` (повторный вызов игнорируется).
   (Первая версия использовала `SloppyDigits` — векторные «рукописные» цифры; заменены по просьбе владельца
   на стандартное начертание.) Крупных букв нет — день недели и месяц набираются `UI_12` bold.

### 4.7. i18n

- Источник: `lib/I18n/translations/*.yaml` (34 языка), английский — эталон.
- `scripts/gen_i18n.py` запускается как **pre-скрипт сборки** (`platformio.ini:89,235`),
  генерирует `I18nKeys.h` / `I18nStrings.{h,cpp}`; они в `.gitignore` — руками не трогать.
- **Отсутствующие ключи подставляются из английского** («missing in `<lang>`, using
  English fallback»). Значит добавить ключ можно в 1–3 языка, остальные 31 не сломаются.
- Все строки UI — только через `tr(STR_…)`, хардкод запрещён правилами проекта.

### 4.8. Сборка

- `build_src_filter = +<*>` в `[base]` (`platformio.ini:75`) — **новые .cpp в `src/`
  подхватываются автоматически**, `platformio.ini` править не нужно.
  (Симулятор на `:200` переопределяет фильтр, но тоже начинается с `+<*>`.)
- Окружения Paper Mono: `[papermono_hardware]` (`:705`), `[env:papermono]` (`:724`,
  dev-сборка с serial-логом), `papermono-gh_release`, `papermono_nightly`.
- Флаг платы: `-DFREEINK_DEVICE_PAPERMONO=1` (`:715`).

---

## 5. Архитектура репозитория

```
crossmux-calendar/               ← ваш репозиторий; кода CrossMux в нём НЕТ
├── CONCEPT.md                   ← этот файл
├── upstream.lock                ← SHA последнего проверенного коммита upstream
├── design/
│   └── calendar-standby-mockup.html
├── overlay/                     ← новые файлы, копируются в клон как есть
│   └── src/activities/apps/standby/
│       ├── CalendarFace.h
│       ├── CalendarFace.cpp
│       ├── CalendarCore.h       ← чистая логика (без Arduino), тестируется на хосте
│       ├── CalendarCore.cpp
│       ├── SunTimes.h/.cpp      ← восход/закат, офлайн
│       └── WeatherClient.h/.cpp ← этап 5
├── hooks/
│   └── apply_hooks.py           ← точечные правки существующих файлов
├── scripts/
│   ├── sync.sh                  ← клон upstream + overlay + hooks
│   ├── build.sh                 ← pio run -e papermono
│   ├── flash.sh                 ← pio run -e papermono -t upload
│   └── sim.sh                   ← pio run -e simulator -t run_simulator
├── tests/                       ← host-тесты CalendarCore против python calendar
└── work/                        ← рабочий клон upstream; в .gitignore, пересоздаётся
```

### 5.1. Что делает `apply_hooks.py`

Три хука, все идемпотентные, все ищут якоря по смыслу (хук 3 — ориентация, описан ниже):

**Хук 1 — `src/activities/apps/standby/StandbyActivity.cpp`:**

```diff
 #include "SloppyClockFace.h"
+#include "CalendarFace.h"  // [calmod]

 constexpr FaceEntry kFaces[] = {
     {[]() -> std::unique_ptr<StandbyFace> { return makeUniqueNoThrow<SloppyClockFace>(); },
      [](int, int) { return true; }},
-#ifdef ENABLE_CHINESE_VERSION
-    {[]() -> std::unique_ptr<StandbyFace> { return makeUniqueNoThrow<ChineseCalendarFace>(); },
-     [](int sw, int sh) { return sh > sw; }},  // portrait only
-#endif
+    {[]() -> std::unique_ptr<StandbyFace> { return makeUniqueNoThrow<CalendarFace>(); },
+     [](int, int) { return true; }},  // [calmod] обе ориентации
 };
```

**Хук 2 — `lib/I18n/translations/{english,russian,german}.yaml`:** дописать
`STR_FACE_CALENDAR` в конец файла. Остальные языки возьмут английский по fallback.

**Хук 3 — `StandbyActivity.cpp`, ориентация.** Upstream применяет `SETTINGS.orientation`
(плитка в шторке / «Настройки») **только в читалке** (`ReaderUtils::applyOrientation`), а `ReaderActivity`
при выходе сбрасывает рендерер в Portrait. Стендбай ориентации не читает — поэтому без хука он всегда
портретный, хотя плитка в шторке переключается (она меняет только настройку: `FrontlightPanelActivity.cpp:172`).
Хук: `calendar_orientation::sync(renderer)` в начале `onEnter()` (после `Activity::onEnter()`, до выбора
грани по размерам) и в начале `loop()` (ловит смену из шторки), `restore()` (Portrait) перед
`Activity::onExit()`. Код — `overlay/.../CalendarOrientation.h`, +5 строк в файле.

Итог на `c92edca`: **4 файла, +10 / −4 строки.**

### 5.2. Что проверено в этой сессии

Скрипт прогнан на свежем клоне `c92edca`, все сценарии:

| Сценарий | Результат |
|---|---|
| Обычное применение | 4 файла пропатчены, +6/−4 |
| Повторный запуск | `unchanged` во всех файлах, ничего не дублируется |
| upstream переименовал `kFaces[]` → `kFaceTable[]` | **exit 1**, `ANCHOR LOST: не найдена таблица kFaces[]` |
| upstream сам убрал строку китайского календаря | корректно: «строки нет, просто добавляем свою» |
| `gen_i18n.py` после хука | 34 языка, 1102 ключа; `STR_FACE_CALENDAR` в `I18nKeys.h`; «Календарь» в русском блоке `I18nStrings.cpp` |

**Что НЕ проверено:** компиляция прошивки (`pio run -e papermono`) и, разумеется,
поведение на железе. Первое, что нужно сделать у себя — §8, этап 0.

### 5.3. Обновление upstream

```bash
./scripts/sync.sh              # берёт SHA из upstream.lock — воспроизводимо
./scripts/sync.sh --latest     # берёт свежий origin/main — проверка на дрейф
```

Алгоритм `sync.sh`: снести `work/`, клонировать upstream (с `--recursive`,
там сабмодуль `freeink-sdk`), сделать `git checkout <ref>`, скопировать `overlay/`,
запустить `hooks/apply_hooks.py work/`.

Дальше `./scripts/build.sh`. Сборка зелёная на свежем `main` → записать новый SHA
в `upstream.lock` и закоммитить. Красная → лог покажет, какой якорь потерялся.

Ночной GitHub Actions на `--latest` ловит дрейф раньше, чем вы о него споткнётесь
(этап 7).

---

## 6. Правила upstream, которые обязательны для нашего кода

Из `AGENTS.md` (полные формулировки — там же и в `docs/engineering/`):

1. **Базовая планка — ESP32-C3: ~380 KB RAM, без PSRAM.** Общий код должен в неё
   влезать, даже если наша плата богаче. Обосновывать каждое выделение в куче,
   предпочитать стек/статику, `.reserve()` перед циклами `push_back`.
2. **Никогда голый `new`.** Сборка с `-fno-exceptions`: неудачный `new` вызывает
   `abort()`, а не возвращает `nullptr`. Использовать `makeUniqueNoThrow<T>()`
   из `lib/Memory/Memory.h`, всегда проверять на null и писать `LOG_ERR` при OOM.
3. **Весь пользовательский текст — через `tr()`.** Логи можно хардкодить.
4. **Только HAL-классы**, не SDK напрямую (`Storage`, `HalDisplay`, `HalGPIO`).
5. **Никаких `file.close()`** для локальных `FsFile` — деструктор закрывает сам.
6. **`memcpy` для невыровненного чтения** (RISC-V падает на `reinterpret_cast`).
7. **Логические кнопки `MappedInputManager::Button::*`**, не сырые `HalGPIO::BTN_*`.
8. **Никогда не хардкодить 800/480** — только `renderer.getScreenWidth()/getScreenHeight()`.
9. **Что выделено в `onEnter()` — освободить в `onExit()`.**
10. **Не править генерируемые файлы** (`*.generated.h`, `I18nKeys.h`, `I18nStrings.*`).
11. **Соседние интерактивные элементы — минимум 6 px видимого зазора**, геометрия
    отрисовки и хит-теста берётся из одного источника.

Перед коммитом: `./bin/clang-format-fix` (нужен clang-format 21+), затем `./bin/ci-check`.

---

## 7. Окружение на Arch Linux

```bash
sudo pacman -S --needed git python python-pipx sdl2 curl openssl clang
pipx install platformio
pipx ensurepath   # затем перелогиниться или source ~/.bashrc
```

**Доступ к последовательному порту** — один из двух путей:

```bash
# путь A: udev-правила PlatformIO (рекомендуется)
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
  | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
# после этого физически переподключить плату

# путь B: группы (на Arch порты принадлежат uucp)
sudo usermod -aG uucp,lock $USER
# обязательно перелогиниться
```

Проверка: `pio device list` — плата должна появиться как `/dev/ttyACM*`.

`clang-format` из репозиториев Arch обычно свежий (пакет `clang`), но убедитесь:
`clang-format --version` ≥ 21, иначе `./bin/ci-check` упадёт на неизвестном
ключе `AlignFunctionDeclarations`.

---

## 8. План по этапам

### Этап 0 — baseline (сначала это, до любого кода)
- [x] `git clone --recursive https://github.com/0x1abin/crossmux.git`
- [x] `pio run -e papermono` — **собирается** (2026-09-20, `c92edca`: RAM 35,4 %, Flash 90,2 % раздела; PlatformIO — в venv `../pio-env`)
- [ ] `pio run -e papermono -t upload` — прошивается? Входит ли плата в загрузчик сама,
      или нужен ручной режим (удержание кнопки при подключении)? *(плата не была подключена)*
- [x] Записать SHA в `upstream.lock`
- [ ] **Сохранить рабочую прошивку** на случай отката — `./scripts/backup.sh` (полный дамп флеша 16 МБ)

### Этап 1 — каркас репозитория
- [x] Структура из §5, `scripts/*.sh`, `hooks/apply_hooks.py`
- [x] `CalendarFace` — заглушку пропустили, сразу полноценная грань
- [x] `sync.sh && build.sh` (хуки: 4 файла, +6/−4, идемпотентны). `flash.sh` — ждёт плату
- [~] **Критерий:** в симуляторе грань доступна (Right в Standby), китайской нет, обе ориентации. На плате — не проверено

### Этап 2 — `CalendarCore` (чистая логика, без Arduino)
- [x] Сетка месяца: первый день недели, дни соседних месяцев, високосные годы
- [x] Номер недели ISO-8601, день года
- [x] Навигация «сегодня ± N месяцев», границы 1970–2100
- [x] Свои таблицы названий дней и месяцев (ru/en/de) — **не** трогать i18n upstream,
      чтобы не плодить правки в генерируемых файлах
- [x] Host-тесты против python `calendar` на диапазоне 1970–2100 (`tests/run.sh`: 47 847 дней, 1 572 сетки, 0 расхождений)
- [x] **Критерий:** тесты зелёные

### Этап 3 — отрисовка по макету
- [x] Выбор раскладки по `getScreenWidth() > getScreenHeight()`
- [x] Крупное время (`SloppyDigits` или своя геометрия — см. §4.6)
- [x] Дата в двух форматах, день недели, чипсы «Нед. N» / «День N / 365»
- [x] Сетка месяца, сегодня инверсией, выходные и соседние месяцы оттенками
- [x] `tick()`: минутный тик для времени, смена суток для сетки;
      `secondsUntilNextWake()` — до следующей минуты
- [~] Полный refresh раз в N обновлений против ghosting (запрос есть — `RedrawWithGhostCleanup` каждые 30 обновлений и в полночь; но upstream выполняет его только на Xteink-платах, на Paper Mono эффекта не будет — решать по факту ghosting на экране)
- [x] `render()` идемпотентен (grayscale не используем — см. §9)
- [x] Up/Down = месяц ∓1, авто-возврат к текущему месяцу по бездействию
- [x] Вид сверен со скриншотами симулятора (`./scripts/shots.sh`): портрет, ландшафт, 5- и 6-строчные месяцы, листание, Immersive
- [ ] **Критерий:** макет воспроизведён **на железе** в обеих ориентациях

### Этап 4 — восход, закат, длина дня
- [x] Расчёт офлайн по широте/долготе (NOAA solar position), без запросов в сеть
- [~] Координаты — константы в `CalendarConfig.h` (`kSunEnabled=false`, пока не вписан город)
- [~] **Критерий:** сверка с независимой реализацией `astral` — 396 точек (11 городов × 12 мес × 3 даты), худшее расхождение 1,73 мин. Сверка с календарём **вашего** города — после ввода координат

### Этап 5 — погода
- [x] Свой цикл выхода в Wi-Fi раз в 30 мин (`kWeatherRefreshMin`) — из `CalendarFace::tick()`, **хук в `loop()` не понадобился** (§4.5)
- [x] Место по IP: `ipwhois.app` (без ключа; название города на языке интерфейса ru/en/de), не чаще раза в 3 ч и сразу при смене языка
- [x] Open-Meteo `api.open-meteo.com/v1/forecast` — без ключа; сейчас: температура, «ощущается», код WMO, ветер, день/ночь; за день: min/max, сумма осадков, вероятность
- [x] Кэш на SD (`/.crosspoint/calendar_cache.json`): **город и последняя погода переживают перезагрузку и отсутствие сети**. Город по умолчанию — Москва (тест), пока IP-определение ни разу не удалось
- [x] Заглушки: каждое недостающее поле — `--`; нет данных совсем — облако с косой чертой и «Нет данных»; старше 3 ч — «устарело · ЧЧ:ММ»; старше 12 ч — значения не показываем
- [ ] Настройки: интервал обновления, «только при зарядке», «город вручную» — **не сделано** (интервалы зашиты в `CalendarConfig.h`)
- [ ] **Критерий:** погода обновляется на плате, расход батареи приемлемый (измерить за сутки) — **на железе не проверено**; в симуляторе (реальная сеть через curl хоста) — работает

Атрибуция: данные Open-Meteo.com, CC BY 4.0, бесплатно для некоммерческого использования.

### Этап 6 — тап по погоде (бэклог, после стабилизации)
- [ ] `onTap(x, y)` в `StandbyFace` + третий хук в `loop()`
- [ ] Прогноз на 3 дня или по часам

### Этап 7 — CI
- [ ] GitHub Actions: ночная сборка `sync.sh --latest && build.sh`
- [ ] Красная сборка = upstream уехал; issue с текстом `ANCHOR LOST`
- [ ] Артефакт `firmware.bin` в релизе

---

## 9. Риски и открытые вопросы

| Риск | Смягчение |
|---|---|
| **Дрейф якорей** в `StandbyActivity.cpp` | Всего 2 хука; падение громкое и адресное; ночной CI ловит рано |
| **Расход батареи** от Wi-Fi каждые 5 мин (`kWeatherRefreshMin`). На S3-целях стендбай работает без light sleep | Настройка интервала, режим «только при зарядке», измерить за сутки |
| **OTA затрёт вашу сборку.** `OtaUpdater` сравнивает `CROSSPOINT_VERSION` семантически | Добавить свой суффикс версии (`-cal`) и/или не пользоваться OTA из меню; проверить логику на `src/network/OtaUpdater.cpp:129…163` |
| **Автовход в загрузчик** при `pio upload` на Paper Mono — не проверен | Этап 0; при неудаче — ручной режим загрузки |
| ~~Кириллица в 18 pt~~ — **подтвердилось, что её нет** (см. §4.6). Грань переведена на `UI_10/UI_12` | Закрыто в симуляторе; на железе сверить глазами |
| ~~Ориентация Standby не настраивалась~~ — **исправлено хуком 3** (§5.1): грань следует `SETTINGS.orientation` (плитка в шторке, все 4 режима проверены в симуляторе) | На плате проверить смену ориентации из шторки, пока стендбай открыт |
| **Grayscale не используем сознательно**: текст рисуется только чёрным/белым, серыми бывают лишь растровые заливки `fillRectDither` (шаблон, не настоящий серый). `wantsGrayscale()` = `false` | Выходные — растр, соседние месяцы — мелкий шрифт; работает одинаково в BW и без 2-секундного gray-прохода каждую минуту |

---

## 10. Бэклог идей (обсуждалось, не в v1)

- Тап по блоку погоды → прогноз на 3 дня / по часам *(согласовано, этап 6)*
- «До выходных: N дн.» или до ближайшего праздника (таблица праздников по стране)
- Качество воздуха и УФ-индекс одной строкой (Open-Meteo Air Quality API)
- Обратный отсчёт до своей даты (отпуск, день рождения) — задаётся в настройках
- «Прочитано сегодня: N мин» из статистики чтения CrossMux
- Wi-Fi-позиционирование вместо IP: beaconDB (бесплатно, API совместим с закрытым
  Mozilla Location Service, но покрытие неравномерное) либо Google Geolocation API
  (нужны ключ и биллинг). **Важно:** при любом Wi-Fi-варианте устройство отправляет
  наружу MAC-адреса соседних сетей; IP-вариант отправляет только IP.
  Для погоды точность до города достаточна — модели Open-Meteo имеют разрешение 1–11 км.

---

## 11. Шпаргалка команд

```bash
# сборка и прошивка Paper Mono
pio run -e papermono                 # dev-сборка (serial-лог, LOG_LEVEL=2)
pio run -e papermono -t upload       # прошить подключённую плату
pio device monitor                   # серийные логи
python3 scripts/debugging_monitor.py # расширенный монитор (pyserial colorama matplotlib)

# бинарник
.pio/build/papermono/firmware.bin

# симулятор (нет отдельного Paper Mono; X4 — те же 800×480)
pio run -e simulator -t run_simulator

# проверки перед коммитом
./bin/clang-format-fix
./bin/ci-check

# наш overlay
./scripts/sync.sh            # клон по upstream.lock + overlay + хуки
./scripts/sync.sh --latest   # то же, но свежий origin/main
./scripts/overlay.sh         # быстро накатить overlay+хуки на готовый work/ (без клонирования)
python3 hooks/apply_hooks.py work/   # только хуки, идемпотентно
./scripts/build.sh           # прошивка → work/.pio/build/papermono/firmware.bin
./scripts/flash.sh           # прошить плату по USB (pio upload)
./tests/run.sh               # host-тесты: календарь, солнце, погода

# симулятор (SDL2, окно): Esc = Back, Enter, стрелки, мышь = тач
SIM_FAKE_WIFI=1 ./scripts/sim.sh      # на главной Esc («Standby») → стрелка Вправо = «Календарь»
SIM_BUILD_ONLY=1 ./scripts/sim.sh     # только собрать
./scripts/shots.sh [каталог] [сценарии]   # скриншоты без окна: nodata moscow live offline stale orient month
```

---

## 12. Источники

- CrossMux: <https://github.com/0x1abin/crossmux> — `AGENTS.md`,
  `docs/engineering/device-variants.md`, `docs/engineering/chinese-build.md`,
  `docs/contributing/getting-started.md`
- M5Stack PaperMono: <https://docs.m5stack.com/en/core/PaperMono>
- Open-Meteo: <https://open-meteo.com/> (без ключа, CC BY 4.0, некоммерческое использование)
- ipwhois.io: <https://ipwhois.io/docs> (бесплатный endpoint без ключа, 1000 запросов/сутки)
- beaconDB: <https://github.com/beacondb/beacondb> (замена Mozilla Location Service)
- Google Geolocation API: <https://developers.google.com/maps/documentation/geolocation/requests-geolocation>
- PlatformIO udev: <https://docs.platformio.org/en/stable/core/installation/udev-rules.html>
