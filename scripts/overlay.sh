#!/usr/bin/env bash
# Быстрая итерация: заново накатить overlay + хуки на уже готовый work/ (без клонирования).
set -euo pipefail
source "$(dirname "$0")/env.sh"
[ -d "$WORK/.git" ] || { echo "work/ нет — сначала ./scripts/sync.sh" >&2; exit 1; }
cp -r "$ROOT/overlay/." "$WORK/"
python3 "$ROOT/hooks/apply_hooks.py" "$WORK"
