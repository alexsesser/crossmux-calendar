#!/usr/bin/env python3
"""Сверка MoonPhase (Meeus) с ephem на всех новолуниях и полнолуниях ~1970–2100. Допуск — 2 минуты."""
import os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "build", "pydeps"))
try:
    import ephem
except ImportError:
    print("ephem не установлен — сверка Луны пропущена (pip install --target tests/build/pydeps ephem)"); sys.exit(0)
out = subprocess.run([sys.argv[1]], capture_output=True, text=True, check=True).stdout.splitlines()
EJD = 2415020.0  # JD для ephem.Date(0) = 1899/12/31 12:00
worst = 0.0; n = bad = 0
for ln in out:
    p = ln.split()
    if p[0] not in "NF": 
        print(ln); continue
    jd = float(p[2])
    d = ephem.Date(jd - EJD)
    ref = ephem.next_new_moon(d - 3) if p[0] == "N" else ephem.next_full_moon(d - 3)   # ближайшее к нашему моменту
    diff = abs(float(ref) + EJD - jd) * 86400
    worst = max(worst, diff); n += 1
    if diff > 120:
        bad += 1
        if bad <= 5: print(f"{p[0]} k={p[1]}: расхождение {diff:.0f} с ({ephem.Date(jd-EJD)} против {ref})")
print(f"событий: {n}, расхождений > 2 мин: {bad}, худшее: {worst:.1f} с")
sys.exit(1 if bad else 0)
