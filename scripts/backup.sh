#!/usr/bin/env bash
# Полный дамп флеша Paper Mono (16 МБ) перед первой прошивкой — путь отката (CONCEPT §8, этап 0).
#   ./scripts/backup.sh [порт]      # по умолчанию /dev/ttyACM0
# Использует esptool из ../esptool-env. Плата должна быть подключена по USB.
set -euo pipefail
source "$(dirname "$0")/env.sh"
ESPTOOL="${ESPTOOL:-$ROOT/../esptool-env/bin/esptool}"
PORT="${1:-/dev/ttyACM0}"
OUT="$ROOT/backups/papermono-flash-$(date +%Y%m%d-%H%M%S).bin"
mkdir -p "$ROOT/backups"
[ -e "$PORT" ] || { echo "порта $PORT нет — плата подключена? (pio device list)" >&2; exit 1; }
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 read-flash 0 0x1000000 "$OUT"
sha256sum "$OUT" | tee "$OUT.sha256"
echo "бэкап: $OUT"
