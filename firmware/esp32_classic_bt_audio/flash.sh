#!/usr/bin/env bash
# Flash the classic ESP32 Bluetooth audio bridge (esp32_audio_bridge) via PlatformIO.
#
# Examples:
#   ./flash.sh --upload-port /dev/cu.usbserial-0001                          # esp32wrover (DevKitC-VE, PSRAM)
#   ENV=audiocarrier ./flash.sh --upload-port /dev/cu.usbserial-0001         # the audio card (AudioCarrier/)
#   ENV=esp32dev ./flash.sh ...                                               # the older WROOM DevKitC-32 (no PSRAM)
set -euo pipefail
cd "$(dirname "$0")"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  sed -n '2,7p' "$0"
  exit 0
fi

# PlatformIO from the display firmware's venv (Python >= 3.10; ../esp32_p4_display/flash.sh creates it).
PY=../esp32_p4_display/.venv/bin/python
if [[ ! -x "${PY}" ]]; then
  echo "No PlatformIO venv yet: run ../esp32_p4_display/flash.sh once (it creates ../esp32_p4_display/.venv)." >&2
  exit 1
fi
# file://../elc_can can stay stuck on an old copy in libdeps; always reinstall it from the repo.
rm -rf .pio/libdeps/*/elc_can 2>/dev/null || true
exec "${PY}" -m platformio run -e "${ENV:-esp32wrover}" -t upload "$@"
