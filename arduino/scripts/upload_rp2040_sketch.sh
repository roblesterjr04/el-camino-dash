#!/usr/bin/env bash
set -euo pipefail

# Compile and upload a Raspberry Pi Pico (RP2040) sketch.
# Supports both:
# - Serial upload (normal runtime USB CDC mode)
# - UF2 mass-storage upload (BOOTSEL mode, RPI-RP2 volume)

ARDUINO_SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sh_common.inc.sh
source "${ARDUINO_SCRIPTS_DIR}/sh_common.inc.sh"

PROJECT_ROOT="$(cd "${ARDUINO_SCRIPTS_DIR}/../.." && pwd)"
SKETCH_DEFAULT="encoder_controller"
# Earle Philhower core (arduino-pico). Mbed arduino:mbed_rp2040:pico is known to RTOS-fault with 4 long / 4 short LED blink.
FQBN_DEFAULT="rp2040:rp2040:rpipico"
FQBN="${FQBN_DEFAULT}"
BUILD_SUBDIR=""

usage() {
  cat <<'USAGE'
Usage: bash arduino/scripts/upload_rp2040_sketch.sh [[<sketch-folder>] <serial-port|/Volumes/RPI-RP2>] [options]
       bash arduino/scripts/upload_rp2040_sketch.sh <serial-port|/Volumes/RPI-RP2> [options]

With no arguments: uploads "encoder_controller". Prefers serial port; falls back to BOOTSEL UF2 drive.

One argument:
  - /dev/...            => serial port
  - /Volumes/... or RPI-RP2 => BOOTSEL UF2 volume
  - anything else       => sketch name, auto-detect upload path

Options:
  --fqbn <id>     Fully qualified board name (default: rp2040:rp2040:rpipico; Mbed: arduino:mbed_rp2040:pico)
  --build-dir <p> Override build output directory (default: arduino/build/<sketch-folder>)

Examples:
  bash arduino/scripts/upload_rp2040_sketch.sh
  bash arduino/scripts/upload_rp2040_sketch.sh /dev/cu.usbmodem1101
  bash arduino/scripts/upload_rp2040_sketch.sh encoder_controller /dev/cu.usbmodem1101
  bash arduino/scripts/upload_rp2040_sketch.sh encoder_controller /Volumes/RPI-RP2
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
UF2_VOLUME=""

detect_bootsel_volume() {
  if [[ -d "/Volumes/RPI-RP2" ]]; then
    printf "%s\n" "/Volumes/RPI-RP2"
    return 0
  fi
  return 1
}

detect_serial_port_safe() {
  if PORT="$(elc_detect_serial_port pico 2>/dev/null)"; then
    return 0
  fi
  PORT=""
  return 1
}

case "${#EXTRA_ARGS[@]}" in
  0)
    SKETCH_NAME="${SKETCH_DEFAULT}"
    detect_serial_port_safe || true
    ;;
  1)
    if [[ "${EXTRA_ARGS[0]}" == /dev/* ]]; then
      SKETCH_NAME="${SKETCH_DEFAULT}"
      PORT="${EXTRA_ARGS[0]}"
    elif [[ "${EXTRA_ARGS[0]}" == /Volumes/* || "${EXTRA_ARGS[0]}" == "RPI-RP2" ]]; then
      SKETCH_NAME="${SKETCH_DEFAULT}"
      UF2_VOLUME="${EXTRA_ARGS[0]}"
      if [[ "${UF2_VOLUME}" == "RPI-RP2" ]]; then
        UF2_VOLUME="/Volumes/RPI-RP2"
      fi
    else
      SKETCH_NAME="${EXTRA_ARGS[0]}"
      detect_serial_port_safe || true
    fi
    ;;
  2)
    SKETCH_NAME="${EXTRA_ARGS[0]}"
    if [[ "${EXTRA_ARGS[1]}" == /dev/* ]]; then
      PORT="${EXTRA_ARGS[1]}"
    elif [[ "${EXTRA_ARGS[1]}" == /Volumes/* || "${EXTRA_ARGS[1]}" == "RPI-RP2" ]]; then
      UF2_VOLUME="${EXTRA_ARGS[1]}"
      if [[ "${UF2_VOLUME}" == "RPI-RP2" ]]; then
        UF2_VOLUME="/Volumes/RPI-RP2"
      fi
    else
      PORT="${EXTRA_ARGS[1]}"
    fi
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

if [[ -z "${PORT}" && -z "${UF2_VOLUME}" ]]; then
  if UF2_VOLUME="$(detect_bootsel_volume)"; then
    echo "No Pico serial port found; using BOOTSEL volume: ${UF2_VOLUME}"
  fi
fi

elc_arduino_core_ensure "${FQBN}"

echo "Compiling ${SKETCH_DIR} (${FQBN})..."
mkdir -p "${BUILD_DIR}"
arduino-cli compile \
  --fqbn "${FQBN}" \
  --build-path "${BUILD_DIR}" \
  --build-property "compiler.cpp.extra_flags=-DENCODER_DO_NOT_USE_INTERRUPTS" \
  "${SKETCH_DIR}"

if [[ -n "${PORT}" ]]; then
  echo "Uploading via serial port ${PORT}..."
  arduino-cli upload --fqbn "${FQBN}" --port "${PORT}" --input-dir "${BUILD_DIR}" "${SKETCH_DIR}"
else
  if [[ -z "${UF2_VOLUME}" ]]; then
    echo "No upload target found." >&2
    echo "Connect Pico in normal mode for serial upload, or BOOTSEL mode for /Volumes/RPI-RP2 UF2 copy." >&2
    exit 1
  fi
  if ! [[ -d "${UF2_VOLUME}" ]]; then
    echo "UF2 volume not found: ${UF2_VOLUME}" >&2
    echo "Put Pico in BOOTSEL mode so it mounts as RPI-RP2." >&2
    exit 1
  fi
  shopt -s nullglob
  uf2_files=("${BUILD_DIR}"/*.uf2)
  shopt -u nullglob
  if [[ ${#uf2_files[@]} -lt 1 ]]; then
    echo "No UF2 artifact found in build dir: ${BUILD_DIR}" >&2
    exit 1
  fi
  uf2_file="${uf2_files[0]}"
  echo "Uploading via UF2 copy to ${UF2_VOLUME} ($(basename "${uf2_file}"))..."
  cp "${uf2_file}" "${UF2_VOLUME}/"
  sync || true
fi

echo "RP2040 upload complete."
