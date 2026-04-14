#!/usr/bin/env bash
# Flash classic ESP32 Bluetooth audio bridge (esp32_audio_bridge) via PlatformIO.
# Requires: pio in PATH, or: python3 -m platformio (pip install platformio).
#
# Examples:
#   ./flash.sh
#   ./flash.sh --upload-port /dev/cu.usbserial-0001
#   ./flash.sh --upload-port /dev/ttyUSB0
set -euo pipefail
cd "$(dirname "$0")"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  cat <<'EOF'
Usage: ./flash.sh [--upload-port DEVICE] [-- ...extra platformio args]

Builds env "esp32dev" and uploads. Pass --upload-port if more than one serial
device is connected.

From repo root you can run:
  bash firmware/esp32_classic_bt_audio/flash.sh --upload-port /dev/cu.usbmodem1101
EOF
  exit 0
fi

if command -v pio >/dev/null 2>&1; then
  exec pio run -e esp32dev -t upload "$@"
else
  exec python3 -m platformio run -e esp32dev -t upload "$@"
fi
