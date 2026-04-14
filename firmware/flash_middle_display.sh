#!/usr/bin/env bash
# MIDDLE display — tach + clock (esp32_p4_center), Waveshare 3.4C env.
# Override port: UPLOAD_PORT=/dev/cu.other ./flash_middle_display.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
PORT="${UPLOAD_PORT:-/dev/cu.usbmodem5ABA0545011}"
exec "$ROOT/esp32_p4_center/flash.sh" -e esp32-p4-center-waveshare3c4 --upload-port "$PORT" "$@"
