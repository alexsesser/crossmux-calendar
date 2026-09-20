#!/usr/bin/env bash
# Сборка (если что-то изменилось) и прошивка Paper Mono по USB.
# Порт PlatformIO находит сам; свой: ./scripts/flash.sh --upload-port /dev/ttyACM0
# Пишет: bootloader 0x0, partitions 0x8000, boot_app0 0xe000, приложение 0x10000. SD-карту и настройки на ней не трогает.
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$WORK" && "$PIO" run -e papermono -t upload "$@"
