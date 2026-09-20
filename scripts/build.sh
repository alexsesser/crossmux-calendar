#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$WORK" && "$PIO" run -e "${ENV:-papermono}" "$@"
echo "→ $WORK/.pio/build/${ENV:-papermono}/firmware.bin"
