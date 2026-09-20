#!/usr/bin/env python3
"""Сверка CalendarCore с python: calendar + datetime.isocalendar() на 1970..2100."""
import calendar
import datetime
import subprocess
import sys

lines = subprocess.run([sys.argv[1]], capture_output=True, text=True, check=True).stdout.splitlines()
errors = []
n_days = n_grids = 0
calendar.setfirstweekday(calendar.MONDAY)

for ln in lines:
    p = ln.split()
    if p[0] == "D":
        y, m, d, wd, doy, iy, wk, diy = map(int, p[1:])
        dt = datetime.date(y, m, d)
        ic = dt.isocalendar()
        exp = (dt.weekday(), dt.timetuple().tm_yday, ic[0], ic[1], 366 if calendar.isleap(y) else 365)
        if (wd, doy, iy, wk, diy) != exp:
            errors.append(f"D {dt}: got {(wd, doy, iy, wk, diy)} expected {exp}")
        n_days += 1
    elif p[0] == "G":
        y, m, rows = int(p[1]), int(p[2]), int(p[3])
        weeks = calendar.Calendar(0).monthdatescalendar(y, m)
        if rows != len(weeks):
            errors.append(f"G {y}-{m}: rows {rows} expected {len(weeks)}")
            continue
        cells = p[4:]
        flat = [dt for w in weeks for dt in w]
        for i, dt in enumerate(flat):
            cy, cm, cd = map(int, cells[i].split(":")[0].split("-"))
            kind, we = map(int, cells[i].split(":")[1:])
            ek = 1 if dt.month == m else (0 if dt < datetime.date(y, m, 1) else 2)
            # до 1970 и после 2100 python-даты корректны, ядро тоже считает их верно
            if (cy, cm, cd) != (dt.year, dt.month, dt.day) or kind != ek or we != (1 if dt.weekday() >= 5 else 0):
                errors.append(f"G {y}-{m} cell {i}: got {cells[i]} expected {dt} kind={ek}")
                break
        n_grids += 1
    elif p[0] == "N1" and p[1] != "0": errors.append("N1: выход за 1970 должен отклоняться")
    elif p[0] == "N2" and p[1] != "0": errors.append("N2: выход за 2100 должен отклоняться")
    elif p[0] == "N3" and p[1:] != ["1", "2025", "12"]: errors.append(f"N3 {p}")
    elif p[0] == "N4" and p[1:] != ["1", "2025", "12"]: errors.append(f"N4 {p}")
    elif p[0] == "N5" and p[1:] != ["1", "2027", "1"]: errors.append(f"N5 {p}")
    elif p[0] == "N6" and p[1:] != ["1", "2028", "12"]: errors.append(f"N6 {p}")

print(f"дней: {n_days}, сеток: {n_grids}, ошибок: {len(errors)}")
for e in errors[:20]:
    print("  ", e)
sys.exit(1 if errors else 0)
