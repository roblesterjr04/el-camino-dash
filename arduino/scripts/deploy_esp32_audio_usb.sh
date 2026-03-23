#!/usr/bin/env bash
set -euo pipefail

# Upload esp32_audio_bridge to an ESP32 board over native USB CDC.
# With no serial port argument, the port is auto-detected (arduino-cli JSON + detect_serial_port.py).

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCR="${ROOT}/arduino/scripts"

usage() {
  cat <<'USAGE'
Usage: bash arduino/scripts/deploy_esp32_audio_usb.sh [<serial-port>] [--fqbn <fqbn>]

Serial port:
  Omit <serial-port> to auto-detect a USB CDC/JTAG serial device (same logic as
  upload_esp32_sketch.sh / detect_serial_port.py).

Defaults to esp32:esp32:esp32 (Bluetooth Classic + A2DP; required for esp32_audio_bridge).
Examples:
  bash arduino/scripts/deploy_esp32_audio_usb.sh
  bash arduino/scripts/deploy_esp32_audio_usb.sh /dev/cu.usbmodem1101
  bash arduino/scripts/deploy_esp32_audio_usb.sh --fqbn esp32:esp32:esp32s3
  bash arduino/scripts/deploy_esp32_audio_usb.sh /dev/cu.usbmodem1101 --fqbn esp32:esp32:esp32s3
USAGE
}

PORT=""
FQBN_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --fqbn)
      FQBN_ARGS=(--fqbn "${2:-}")
      shift 2
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      if [[ -n "${PORT}" ]]; then
        echo "Unexpected extra argument: $1" >&2
        usage >&2
        exit 2
      fi
      PORT="$1"
      shift
      ;;
  esac
done

if [[ ${#FQBN_ARGS[@]} -eq 0 ]]; then
  FQBN_ARGS=(--fqbn esp32:esp32:esp32)
fi

# Do not pass an empty port: upload_esp32_sketch.sh only auto-detects when given
# zero positional arguments (or sketch name without /dev/...).
if [[ -z "${PORT}" ]]; then
  exec bash "${SCR}/upload_esp32_sketch.sh" "${FQBN_ARGS[@]}"
else
  exec bash "${SCR}/upload_esp32_sketch.sh" esp32_audio_bridge "${PORT}" "${FQBN_ARGS[@]}"
fi
