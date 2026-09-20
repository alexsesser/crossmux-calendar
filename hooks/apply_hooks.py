#!/usr/bin/env python3
"""Идемпотентные правки существующих файлов CrossMux под overlay «Календарь».

Ищет якоря по смыслу, а не по номерам строк. Повторный запуск ничего не меняет.
Если upstream изменил форму якоря — падает с ANCHOR LOST и называет место.

    python3 hooks/apply_hooks.py <путь-к-клону-crossmux>

Проверено на 0x1abin/crossmux @ c92edca (2026-09-20).
"""

import os
import pathlib
import re
import sys

MARK = "[calmod]"  # метка идемпотентности: есть в файле -> уже пропатчено
MARK_ORIENT = "[calmod-orient]"  # хук 3: ориентация Standby

# Заголовок грани в UI. Остальные 30+ языков подхватят английский по fallback
# (gen_i18n.py: "missing in <lang>, using English fallback").
I18N_KEY = "STR_FACE_CALENDAR"
I18N_VALUES = {
    "english.yaml": "Calendar",
    "russian.yaml": "Календарь",
    "german.yaml": "Kalender",
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
    for fname, value in I18N_VALUES.items():
        patch(root, f"lib/I18n/translations/{fname}", yaml_key(I18N_KEY, value))
    print("Готово.")


if __name__ == "__main__":
    main()
