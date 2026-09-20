# Общие настройки скриптов (source, не запускать).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
WORK="$ROOT/work"
# PlatformIO живёт в venv рядом с проектом (pipx на этой машине не стоит).
PIO_ENV="${PIO_ENV:-$ROOT/../pio-env}"
PIO="${PIO:-$PIO_ENV/bin/pio}"
[ -x "$PIO" ] || PIO="$(command -v pio || true)"
[ -n "$PIO" ] || { echo "pio не найден: python3 -m venv ../pio-env && ../pio-env/bin/pip install platformio" >&2; exit 1; }
# Бинарник симулятора (sim.sh собирает Paper Mono в отдельный каталог; SIM_DEVICE=x4 — штатный)
if [ "${SIM_DEVICE:-papermono}" = papermono ]; then SIM_BIN="$WORK/.pio/build-sim-pm/simulator/program"; else SIM_BIN="$WORK/.pio/build/simulator/program"; fi
