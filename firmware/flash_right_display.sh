#!/usr/bin/env bash
# RIGHT display — TSPS cluster (esp32_p4_tsps), Waveshare 3.4C env.
# Override port: UPLOAD_PORT=/dev/cu.other ./flash_right_display.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
PORT="${UPLOAD_PORT:-/dev/cu.usbmodem5ABA0545011}"
exec "$ROOT/esp32_p4_tsps/flash.sh" -e esp32-p4-tsps-waveshare3c4 --upload-port "$PORT" "$@"
