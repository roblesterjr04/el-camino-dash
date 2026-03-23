#!/usr/bin/env bash
set -euo pipefail

# Compile and upload an ESP32 sketch over native USB (CDC/JTAG serial), using arduino-cli + esptool.

ARDUINO_SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sh_common.inc.sh
source "${ARDUINO_SCRIPTS_DIR}/sh_common.inc.sh"

PROJECT_ROOT="$(cd "${ARDUINO_SCRIPTS_DIR}/../.." && pwd)"
SKETCH_DEFAULT="esp32_audio_bridge"
FQBN_DEFAULT="esp32:esp32:esp32s3"
FQBN="${FQBN_DEFAULT}"
BUILD_SUBDIR=""

usage() {
  cat <<'USAGE'
Usage: bash arduino/scripts/upload_esp32_sketch.sh [[<sketch-folder>] <serial-port>] [options]
       bash arduino/scripts/upload_esp32_sketch.sh <serial-port> [options]

With no arguments: uploads "esp32_audio_bridge" using the first suitable USB serial port.

One argument: if it looks like a device path (/dev/...), it is the port and the sketch defaults
to esp32_audio_bridge; otherwise it is the sketch folder and the port is auto-detected.

Options:
  --fqbn <id>     Fully qualified board name (default: esp32:esp32:esp32s3)
  --build-dir <p> Override build output directory (default: arduino/build/<sketch-folder>)

Examples:
  bash arduino/scripts/upload_esp32_sketch.sh
  bash arduino/scripts/upload_esp32_sketch.sh /dev/cu.usbmodem1101
  bash arduino/scripts/upload_esp32_sketch.sh esp32_audio_bridge /dev/cu.usbmodem1101
  bash arduino/scripts/upload_esp32_sketch.sh esp32_audio_bridge /dev/cu.usbmodem1101 --fqbn esp32:esp32:esp32c3

Discover ports:  arduino-cli board list
Discover FQBNs:   arduino-cli board listall | grep -i esp32
USAGE
}

EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --fqbn)
      FQBN="${2:-}"
      shift 2
      ;;
    --build-dir)
      BUILD_SUBDIR="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      EXTRA_ARGS+=("$1")
      shift
      ;;
  esac
done

SKETCH_NAME=""
PORT=""

case "${#EXTRA_ARGS[@]}" in
  0)
    SKETCH_NAME="${SKETCH_DEFAULT}"
    PORT="$(elc_detect_serial_port esp32)"
    ;;
  1)
    if [[ "${EXTRA_ARGS[0]}" == /dev/* ]]; then
      SKETCH_NAME="${SKETCH_DEFAULT}"
      PORT="${EXTRA_ARGS[0]}"
    else
      SKETCH_NAME="${EXTRA_ARGS[0]}"
      PORT="$(elc_detect_serial_port esp32)"
    fi
    ;;
  2)
    SKETCH_NAME="${EXTRA_ARGS[0]}"
    PORT="${EXTRA_ARGS[1]}"
    ;;
  *)
    echo "Too many positional arguments." >&2
    usage >&2
    exit 2
    ;;
esac

SKETCH_DIR="${PROJECT_ROOT}/arduino/sketches/${SKETCH_NAME}"

if [[ -z "${BUILD_SUBDIR}" ]]; then
  BUILD_DIR="${PROJECT_ROOT}/arduino/build/${SKETCH_NAME}"
else
  BUILD_DIR="${BUILD_SUBDIR}"
fi

if ! [[ -d "${SKETCH_DIR}" ]]; then
  echo "Sketch directory not found: ${SKETCH_DIR}" >&2
  exit 1
fi

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "arduino-cli not found. Run: bash arduino/scripts/install_arduino_cli.sh" >&2
  exit 1
fi

elc_arduino_core_ensure "${FQBN}"

echo "Compiling ${SKETCH_DIR} (${FQBN})..."
mkdir -p "${BUILD_DIR}"
arduino-cli compile --fqbn "${FQBN}" --build-path "${BUILD_DIR}" "${SKETCH_DIR}"

echo "Uploading to ${PORT}..."
arduino-cli upload --fqbn "${FQBN}" --port "${PORT}" --input-dir "${BUILD_DIR}" "${SKETCH_DIR}"

echo "ESP32 upload complete."
