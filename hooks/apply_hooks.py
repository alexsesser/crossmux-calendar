#!/usr/bin/env python3
"""Идемпотентные правки существующих файлов CrossMux под overlay «Календарь».

Ищет якоря по смыслу, а не по номерам строк. Повторный запуск ничего не меняет.
Если upstream изменил форму якоря — падает с ANCHOR LOST и называет место.

    python3 hooks/apply_hooks.py <путь-к-клону-crossmux>

Проверено на 0x1abin/crossmux @ c92edca (2026-09-20). Хуки 1–4 — грань «Календарь», 5–9 — Wi-Fi Enterprise (CONCEPT §13.5).
"""

import os
import pathlib
import re
import sys

MARK = "[calmod]"  # метка идемпотентности: есть в файле -> уже пропатчено
MARK_ORIENT = "[calmod-orient]"  # хук 3: ориентация Standby
MARK_INPUT = "[calmod-input]"    # хук 4: перехват ввода вложенными экранами
MARK_WIFI = "[calmod-wifi]"      # хуки 5–9: Wi-Fi Enterprise (логин + пароль)

# Заголовок грани в UI. Остальные 30+ языков подхватят английский по fallback
# (gen_i18n.py: "missing in <lang>, using English fallback").
I18N = {
    "STR_FACE_CALENDAR": {
        "english.yaml": "Calendar",
        "russian.yaml": "Календарь",
        "german.yaml": "Kalender",
    },
    # Wi-Fi Enterprise: запрос логина перед паролем. Заголовок клавиатуры рисуется шрифтом без кириллицы (тот же
    # NOTOSANS_*, что и в CONCEPT §4.6), поэтому русская строка — латиницей: иначе в заголовке остался бы один «Wi-Fi»,
    # неотличимый от запроса пароля.
    "STR_ENTER_WIFI_USERNAME": {
        "english.yaml": "Enter Wi-Fi username",
        "russian.yaml": "Wi-Fi login",
        "german.yaml": "WLAN-Benutzername",
    },
}


def patch(root: pathlib.Path, rel: str, fn) -> None:
    p = root / rel
    if not p.is_file():
        sys.exit(f"ANCHOR LOST: файла нет: {rel}")
    src = p.read_text(encoding="utf-8")
    out = fn(src)
    if out != src:
        p.write_text(out, encoding="utf-8")
        print(f"  patched   {rel}")
    else:
        print(f"  unchanged {rel}")


CONFIG = pathlib.Path(
    os.environ.get("CALMOD_CONFIG")
    or pathlib.Path(__file__).resolve().parent.parent / "overlay/src/activities/apps/standby/CalendarConfig.h"
)


def calendar_first() -> bool:
    """kCalendarFirstFace из CalendarConfig.h — единственного файла настроек. Нет файла/константы — True."""
    try:
        m = re.search(r"constexpr\s+bool\s+kCalendarFirstFace\s*=\s*(true|false)\s*;", CONFIG.read_text(encoding="utf-8"))
    except OSError:
        return True
    return m is None or m.group(1) == "true"


def standby_activity(s: str) -> str:
    """Подключить CalendarFace и заменить им строку китайского календаря в kFaces[]."""
    if MARK in s:
        return s

    inc = '#include "SloppyClockFace.h"\n'
    if s.count(inc) != 1:
        sys.exit(f'ANCHOR LOST: ожидался ровно один {inc.strip()} в StandbyActivity.cpp')
    s = s.replace(inc, inc + f'#include "CalendarFace.h"  // {MARK}\n')

    i = s.find("constexpr FaceEntry kFaces[] = {")
    e = s.find("\n};", i)
    if i < 0 or e < 0:
        sys.exit("ANCHOR LOST: не найдена таблица kFaces[] в StandbyActivity.cpp")

    table = s[i:e]
    # Убрать #ifdef-блок с ChineseCalendarFace, если он ещё на месте.
    table, n = re.subn(
        r"#ifdef ENABLE_CHINESE_VERSION\n"
        r"(?:(?!#endif)[^\n]*\n)*?[^\n]*\bChineseCalendarFace\b[^\n]*\n"
        r"(?:(?!#endif)[^\n]*\n)*?#endif",
        "",
        table,
    )
    print("    -> строка ChineseCalendarFace удалена" if n
          else "    -> строки ChineseCalendarFace нет, просто добавляем свою")
    table = re.sub(r"\n{2,}", "\n", table).rstrip("\n")

    # Порядок граней задаёт kCalendarFirstFace (CalendarConfig.h): true — календарь первой строкой таблицы
    # (StandbyActivity::onEnter всегда открывает грань 0), false — после Sloppy Clock. Left/Right листают между ними.
    first = calendar_first()
    row = (
        "\n    {[]() -> std::unique_ptr<StandbyFace> { return makeUniqueNoThrow<CalendarFace>(); },\n"
        f"     [](int, int) {{ return true; }}}},  // {MARK} обе ориентации{', первая грань' if first else ''}"
    )
    head = "constexpr FaceEntry kFaces[] = {"
    if not table.startswith(head):
        sys.exit("ANCHOR LOST: таблица kFaces[] начинается не так, как ожидалось")
    print(f"    -> календарь {'первой' if first else 'второй'} гранью (kCalendarFirstFace)")
    if first:
        return s[:i] + head + row + table[len(head):] + s[e:]
    return s[:i] + table + row + s[e:]


def standby_orientation(s: str) -> str:
    """Хук 3: Standby применяет SETTINGS.orientation (upstream делает это только в читалке).

    onEnter: сразу после Activity::onEnter() (до выбора грани по размерам экрана);
    loop:    в начале (ловим смену ориентации из шторки, пока Standby открыт);
    onExit:  вернуть Portrait — остальные экраны рассчитаны на него (так же поступает ReaderActivity).
    """
    if MARK_ORIENT in s:
        return s

    inc = f'#include "CalendarFace.h"  // {MARK}\n'
    if s.count(inc) != 1:
        sys.exit("ANCHOR LOST: нет include CalendarFace.h — хук 1 должен быть применён раньше хука 3")
    s = s.replace(inc, inc + f'#include "CalendarOrientation.h"  // {MARK_ORIENT}\n')

    def add_after(src: str, header: str, line: str, addition: str, before: bool = False) -> str:
        i = src.find(header)
        if i < 0:
            sys.exit(f"ANCHOR LOST: не найдена функция {header.strip()} в StandbyActivity.cpp")
        end = src.find("\n}\n", i)  # конец функции верхнего уровня
        j = src.find(line, i, end)
        if j < 0:
            sys.exit(f"ANCHOR LOST: в {header.strip()} нет строки {line.strip()}")
        if before:
            return src[:j] + addition + src[j:]
        k = j + len(line)
        return src[:k] + addition + src[k:]

    s = add_after(s, "void StandbyActivity::onEnter() {", "  Activity::onEnter();",
                  f"\n  calendar_orientation::sync(renderer);  // {MARK_ORIENT}")
    s = add_after(s, "void StandbyActivity::onExit() {", "  Activity::onExit();",
                  f"  calendar_orientation::restore(renderer);  // {MARK_ORIENT}\n", before=True)
    i = s.find("void StandbyActivity::loop() {")
    if i < 0:
        sys.exit("ANCHOR LOST: не найдена StandbyActivity::loop()")
    k = i + len("void StandbyActivity::loop() {")
    s = s[:k] + f"\n  if (calendar_orientation::sync(renderer)) requestUpdate();  // {MARK_ORIENT}" + s[k:]
    return s


def standby_input(s: str) -> str:
    """Хук 4: вложенные экраны (погода, день, год) перехватывают ввод ДО активности.

    Стендбай сам разбирает тап (инверсия, координаты не передаёт), свайпы ←/→ (смена граней) и Back (выход),
    поэтому иначе экран «Погода» нельзя ни открыть тапом, ни закрыть Back'ом, ни листать свайпом.
    Ставится сразу после хука ориентации в начале StandbyActivity::loop(); CalendarFace::handleInput возвращает
    true, если событие поглощено (тогда активность ничего больше не делает).
    """
    if MARK_INPUT in s:
        return s
    anchor = f"  if (calendar_orientation::sync(renderer)) requestUpdate();  // {MARK_ORIENT}"
    if s.count(anchor) != 1:
        sys.exit("ANCHOR LOST: хук ввода ставится после хука ориентации в StandbyActivity::loop() — его нет")
    add = (
        f"\n  if (CalendarFace::handleInput(mappedInput, mode_ == DisplayMode::Immersive)) {{  // {MARK_INPUT}"
        "\n    lastInputMs_ = millis();"
        "\n    mode_ = DisplayMode::Normal;"
        "\n    requestUpdate();"
        "\n    return;"
        "\n  }"
    )
    return s.replace(anchor, anchor + add, 1)


# ---- Wi-Fi Enterprise (хуки 5–9) -----------------------------------------------------------------------------------

WIFI_INC = f'#include "CalmodWifi.h"  // {MARK_WIFI}\n'
STORE_INC = '#include "WifiCredentialStore.h"\n'
# WiFi.begin(<ssid>.c_str(), <пароль>.c_str()) — в четырёх файлах upstream форма одна и та же.
BEGIN_RE = re.compile(r"WiFi\.begin\((\S+?\.c_str\(\)),\s*(\S+?\.c_str\(\))\);")


def add_wifi_include(s: str, fname: str) -> str:
    if s.count(STORE_INC) != 1:
        sys.exit(f'ANCHOR LOST: в {fname} ожидался ровно один {STORE_INC.strip()}')
    return s.replace(STORE_INC, STORE_INC + WIFI_INC, 1)


def wifi_begin_only(fname: str):
    """WiFi.begin(ssid, пароль) → calmod_wifi::begin(ssid, пароль): Enterprise-строка уйдёт в PEAP."""
    def fn(s: str) -> str:
        if MARK_WIFI in s:
            return s
        s = add_wifi_include(s, fname)
        s, n = BEGIN_RE.subn(rf"calmod_wifi::begin(\1, \2);  // {MARK_WIFI}", s)
        if n == 0:
            sys.exit(f"ANCHOR LOST: в {fname} нет WiFi.begin(ssid.c_str(), пароль.c_str())")
        return s
    return fn


# Точный текст promptPasswordEntry() в upstream: заменяем его целиком, поэтому при любом изменении — ANCHOR LOST.
PROMPT_OLD = """void WifiSelectionActivity::promptPasswordEntry() {
  // Show password entry
  state = WifiSelectionState::PASSWORD_ENTRY;
  // Don't allow screen updates while changing activity
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          state = WifiSelectionState::NETWORK_LIST;
        } else {
          enteredPassword = std::get<KeyboardResult>(result.data).text;
          // state will be updated in next loop iteration
        }
      },
      tr(STR_ENTER_WIFI_PASSWORD), "", 64, InputType::Text);
}
"""
PROMPT_NEW = f"""void WifiSelectionActivity::promptPasswordEntry() {{
  // Show password entry
  state = WifiSelectionState::PASSWORD_ENTRY;
  // {MARK_WIFI} Сеть WPA-Enterprise: сначала логин; loop() увидит takeUserEntered() и снова вызовет эту функцию — уже за паролем.
  if (calmod_wifi::needUsername(selectedSSID)) {{
    startActivityForResultWith<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {{
          if (result.isCancelled || std::get<KeyboardResult>(result.data).text.empty()) {{
            calmod_wifi::clearPending();
            state = WifiSelectionState::NETWORK_LIST;
          }} else {{
            calmod_wifi::setUser(selectedSSID, std::get<KeyboardResult>(result.data).text);
          }}
        }},
        tr(STR_ENTER_WIFI_USERNAME), "", 64, InputType::Text);
    return;
  }}
  // Don't allow screen updates while changing activity
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {{
        if (result.isCancelled) {{
          calmod_wifi::clearPending();  // {MARK_WIFI}
          state = WifiSelectionState::NETWORK_LIST;
        }} else {{
          enteredPassword = calmod_wifi::finishPassword(selectedSSID, std::get<KeyboardResult>(result.data).text);  // {MARK_WIFI}
          // state will be updated in next loop iteration
        }}
      }},
      tr(STR_ENTER_WIFI_PASSWORD), "", 64, InputType::Text);
}}
"""


def wifi_selection(s: str) -> str:
    """Хук 5: экран выбора сети — Enterprise по скану, логин перед паролем, PEAP при подключении, дольше ждём."""
    if MARK_WIFI in s:
        return s
    fname = "WifiSelectionActivity.cpp"
    s = add_wifi_include(s, fname)

    # (а) скан: запомнить Enterprise-сети. Вставка сразу после чтения RSSI в цикле processWifiScanResults().
    anchor = "    const int32_t rssi = WiFi.RSSI(i);\n"
    if s.count(anchor) != 1:
        sys.exit(f"ANCHOR LOST: в {fname} нет цикла скана (const int32_t rssi = WiFi.RSSI(i);)")
    s = s.replace(anchor, anchor + f"    calmod_wifi::noteScan(i, ssid, WiFi.encryptionType(i));  // {MARK_WIFI}\n", 1)

    # (б) запрос логина перед паролем
    if s.count(PROMPT_OLD) != 1:
        sys.exit(f"ANCHOR LOST: в {fname} изменилась promptPasswordEntry()")
    s = s.replace(PROMPT_OLD, PROMPT_NEW, 1)

    # (в) loop(): после логина — второй шаг (пароль) вместо подключения
    anchor = ("  if (state == WifiSelectionState::PASSWORD_ENTRY) {\n"
              "    // Reach here once password entry finished in subactivity\n")
    if s.count(anchor) != 1:
        sys.exit(f"ANCHOR LOST: в {fname}::loop() нет ветки PASSWORD_ENTRY")
    s = s.replace(anchor, anchor + f"    if (calmod_wifi::takeUserEntered()) {{  // {MARK_WIFI} логин введён — теперь пароль\n"
                  "      promptPasswordEntry();\n      return;\n    }\n", 1)

    # (г) подключение
    s, n = BEGIN_RE.subn(rf"calmod_wifi::begin(\1, \2);  // {MARK_WIFI}", s)
    if n != 1:
        sys.exit(f"ANCHOR LOST: в {fname} ожидался один WiFi.begin(ssid, пароль), найдено {n}")

    # (д) ожидание подключения
    old = "  const unsigned long timeoutMs = autoConnecting ? AUTO_CONNECTION_TIMEOUT_MS : CONNECTION_TIMEOUT_MS;\n"
    if s.count(old) != 1:
        sys.exit(f"ANCHOR LOST: в {fname} нет расчёта timeoutMs")
    s = s.replace(old, "  const unsigned long timeoutMs = calmod_wifi::timeoutMs(  // " + MARK_WIFI + "\n"
                  "      autoConnecting ? AUTO_CONNECTION_TIMEOUT_MS : CONNECTION_TIMEOUT_MS);\n", 1)
    return s


def credential_limit(s: str) -> str:
    """Хук 9: логин + пароль лежат одной строкой в поле пароля — лимит 64 байта на неё мал (до 64 + 64 + 2)."""
    if MARK_WIFI in s:
        return s
    old = "inline constexpr size_t MAX_PASSWORD_BYTES = 64;"
    if s.count(old) != 1:
        sys.exit("ANCHOR LOST: в CredentialIntegrity.h нет MAX_PASSWORD_BYTES = 64")
    return s.replace(old, f"inline constexpr size_t MAX_PASSWORD_BYTES = 160;  // {MARK_WIFI} логин (≤64) + пароль (≤64) + разделители", 1)


def yaml_key(key: str, value: str):
    """Дописать ключ в конец YAML, если его там ещё нет."""
    def fn(s: str) -> str:
        if re.search(rf"^{re.escape(key)}\s*:", s, re.M):
            return s
        sep = "" if s.endswith("\n") else "\n"
        return f'{s}{sep}{key}: "{value}"\n'
    return fn


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = pathlib.Path(sys.argv[1]).resolve()
    if not (root / "platformio.ini").is_file():
        sys.exit(f"Не похоже на клон CrossMux: {root}")

    print(f"Применяю хуки в {root}")
    patch(root, "src/activities/apps/standby/StandbyActivity.cpp", standby_activity)
    patch(root, "src/activities/apps/standby/StandbyActivity.cpp", standby_orientation)
    patch(root, "src/activities/apps/standby/StandbyActivity.cpp", standby_input)
    patch(root, "src/activities/network/WifiSelectionActivity.cpp", wifi_selection)
    patch(root, "src/activities/apps/standby/StandbyActivity.cpp", wifi_begin_only("StandbyActivity.cpp"))
    patch(root, "src/activities/apps/pixel-switch/PixelSwitchActivity.cpp", wifi_begin_only("PixelSwitchActivity.cpp"))
    patch(root, "src/activities/apps/airpage/AirPageConnection.cpp", wifi_begin_only("AirPageConnection.cpp"))
    patch(root, "lib/Serialization/CredentialIntegrity.h", credential_limit)
    for key, values in I18N.items():
        for fname, value in values.items():
            patch(root, f"lib/I18n/translations/{fname}", yaml_key(key, value))
    print("Готово.")


if __name__ == "__main__":
    main()
