#!/usr/bin/env bash
# Клон upstream (по upstream.lock или --latest) + overlay + хуки.
set -euo pipefail
source "$(dirname "$0")/env.sh"

REF="$(tr -d '[:space:]' < "$ROOT/upstream.lock")"
[ "${1:-}" = "--latest" ] && REF="origin/main"

rm -rf "$WORK"
git clone --recursive https://github.com/0x1abin/crossmux.git "$WORK"
git -C "$WORK" checkout -q "$REF"
git -C "$WORK" submodule update --init --recursive -q
echo "upstream: $(git -C "$WORK" rev-parse HEAD)"

cp -r "$ROOT/overlay/." "$WORK/"
python3 "$ROOT/hooks/apply_hooks.py" "$WORK"
