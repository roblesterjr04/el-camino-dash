#!/usr/bin/env bash
# Any round display: the same firmware on all three (esp32_p4_display). The board finds its position (left,
# center, right) from the strap on the DigiDash board at boot; nothing to choose here.
# Default env: Waveshare 3.4C. Other panels: ENV=display-waveshare4c ./flash_display.sh
# Override port: UPLOAD_PORT=/dev/cu.other ./flash_display.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
PORT="${UPLOAD_PORT:-/dev/cu.usbmodem5ABA0545011}"
exec "$ROOT/esp32_p4_display/flash.sh" -e "${ENV:-display-waveshare3c4}" --upload-port "$PORT" "$@"
