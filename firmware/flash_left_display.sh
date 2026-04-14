#!/usr/bin/env bash
# LEFT display — speedometer (esp32_p4_speedo), Waveshare 3.4C env.
# Override port: UPLOAD_PORT=/dev/cu.other ./flash_left_display.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
PORT="${UPLOAD_PORT:-/dev/cu.usbmodem5ABA0545011}"
exec "$ROOT/esp32_p4_speedo/flash.sh" -e esp32-p4-speedo-waveshare3c4 --upload-port "$PORT" "$@"
