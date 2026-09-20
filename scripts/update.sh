#!/usr/bin/env bash
# Проверка новой версии upstream: свежий origin/main + overlay + хуки → тесты → сборка.
# Только если ВСЁ зелёное, SHA записывается в upstream.lock. При любой ошибке upstream.lock не меняется
# (work/ при этом остаётся на новом upstream; вернуться к проверенному: ./scripts/sync.sh).
#   ./scripts/update.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"

OLD="$(tr -d '[:space:]' < "$ROOT/upstream.lock")"
"$ROOT/scripts/sync.sh" --latest
NEW="$(git -C "$WORK" rev-parse HEAD)"
if [ "$NEW" = "$OLD" ]; then
  echo "upstream не менялся ($OLD) — обновлять нечего."
  exit 0
fi
echo "upstream: ${OLD:0:8} → ${NEW:0:8}"
git -C "$WORK" log --oneline "$OLD..$NEW" 2>/dev/null | head -20 || true

"$ROOT/tests/run.sh"
"$ROOT/scripts/build.sh"

echo "$NEW" > "$ROOT/upstream.lock"
echo "Готово: сборка зелёная, upstream.lock = $NEW."
echo "Дальше: ./scripts/flash.sh (и, если хотите, SIM_BUILD_ONLY=1 ./scripts/sim.sh && ./scripts/shots.sh для проверки глазами)."
