# Общие настройки скриптов (source, не запускать).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
WORK="$ROOT/work"
# PlatformIO живёт в venv рядом с проектом (pipx на этой машине не стоит).
PIO_ENV="${PIO_ENV:-$ROOT/../pio-env}"
PIO="${PIO:-$PIO_ENV/bin/pio}"
[ -x "$PIO" ] || PIO="$(command -v pio || true)"
[ -n "$PIO" ] || { echo "pio не найден: python3 -m venv ../pio-env && ../pio-env/bin/pip install platformio" >&2; exit 1; }
