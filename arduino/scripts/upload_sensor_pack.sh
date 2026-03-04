#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKETCH_DIR="${PROJECT_ROOT}/arduino/sketches/sensor_pack"
PORT="${1:-}"
FQBN="${2:-arduino:avr:uno}"

if [[ -z "${PORT}" ]]; then
  echo "Usage: bash arduino/scripts/upload_sensor_pack.sh <serial-port> [fqbn]" >&2
  echo "Example: bash arduino/scripts/upload_sensor_pack.sh /dev/cu.usbmodem101 arduino:avr:uno" >&2
  exit 2
fi

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "arduino-cli not found. Run: bash arduino/scripts/install_arduino_cli.sh" >&2
  exit 1
fi

echo "Compiling before upload..."
bash "${PROJECT_ROOT}/arduino/scripts/compile_sensor_pack.sh" "${FQBN}"

echo "Uploading to ${PORT} (${FQBN})"
arduino-cli upload \
  --fqbn "${FQBN}" \
  --port "${PORT}" \
  "${SKETCH_DIR}"

echo "Upload complete."
