#!/usr/bin/env bash
# Клон upstream (по upstream.lock или --latest) + overlay + хуки → work/.
# Клон ПОВЕРХНОСТНЫЙ (одна ревизия, без истории): в разы меньше и быстрее полного. Если сервер не отдаёт нужный
# SHA поверхностно — автоматически откатывается на полный клон.
set -euo pipefail
source "$(dirname "$0")/env.sh"

URL=https://github.com/0x1abin/crossmux.git
REF="$(tr -d '[:space:]' < "$ROOT/upstream.lock")"
[ "${1:-}" = "--latest" ] && REF=""

rm -rf "$WORK"
if [ -z "$REF" ]; then
  git clone -q --depth 1 --recurse-submodules --shallow-submodules "$URL" "$WORK"
else
  git init -q "$WORK"
  git -C "$WORK" remote add origin "$URL"
  if git -C "$WORK" fetch -q --depth 1 origin "$REF" \
     && git -C "$WORK" checkout -q FETCH_HEAD \
     && git -C "$WORK" submodule update -q --init --recursive --depth 1; then
    :
  else
    echo "поверхностный клон на $REF не удался — полный клон" >&2
    rm -rf "$WORK"
    git clone -q --recursive "$URL" "$WORK"
    git -C "$WORK" checkout -q "$REF"
    git -C "$WORK" submodule update -q --init --recursive
  fi
fi
echo "upstream: $(git -C "$WORK" rev-parse HEAD)"

cp -r "$ROOT/overlay/." "$WORK/"
python3 "$ROOT/hooks/apply_hooks.py" "$WORK"
