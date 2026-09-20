#!/usr/bin/env python3
"""Сверка SunTimes (NOAA) с независимой реализацией astral. Допуск по концепции — 2 мин."""
import datetime as dt
import subprocess
import sys

sys.path.insert(0, __import__("os").path.join(__import__("os").path.dirname(__file__), "build", "pydeps"))
try:
    from astral import LocationInfo
    from astral.sun import sunrise, sunset
except ImportError:
    print("astral не установлен — сверка солнца пропущена (pip install --target tests/build/pydeps astral)")
    sys.exit(0)

TOL = 2
worst = 0
n = bad = 0
for ln in subprocess.run([sys.argv[1]], capture_output=True, text=True, check=True).stdout.splitlines():
    p = ln.split()
    name, lat, lon, off = p[0], float(p[1]), float(p[2]), int(p[3])
    y, m, d, valid, pday, pnight, rise, sset = map(int, p[4:12])
    tz = dt.timezone(dt.timedelta(minutes=off))
    try:
        obs = LocationInfo(name, "", "UTC", lat, lon).observer
        s = {"sunrise": sunrise(obs, date=dt.date(y, m, d), tzinfo=tz),
             "sunset": sunset(obs, date=dt.date(y, m, d), tzinfo=tz)}
    except ValueError:
        if not (pday or pnight):
            print(f"{name} {y}-{m:02}-{d:02}: astral говорит полярно, у нас нет"); bad += 1
        n += 1
        continue
    if pday or pnight:
        print(f"{name} {y}-{m:02}-{d:02}: у нас полярно, astral считает"); bad += 1; n += 1
        continue
    er = s["sunrise"].hour * 60 + s["sunrise"].minute + s["sunrise"].second / 60
    es = s["sunset"].hour * 60 + s["sunset"].minute + s["sunset"].second / 60
    dr = min(abs(rise - er), 1440 - abs(rise - er)); ds = min(abs(sset - es), 1440 - abs(sset - es))
    worst = max(worst, dr, ds)
    n += 1
    if dr > TOL or ds > TOL:
        bad += 1
        print(f"{name} {y}-{m:02}-{d:02}: ours {rise//60:02}:{rise%60:02}/{sset//60:02}:{sset%60:02} astral {er//60:02.0f}:{er%60:05.2f}/{es//60:02.0f}:{es%60:05.2f}")
print(f"точек: {n}, расхождений > {TOL} мин: {bad}, худшее: {worst:.2f} мин")
sys.exit(1 if bad else 0)
