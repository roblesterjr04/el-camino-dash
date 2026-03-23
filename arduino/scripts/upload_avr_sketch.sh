#!/usr/bin/env bash
set -euo pipefail

# Local compile + upload for AVR sketches (Uno/Nano and similar) via serial or ICSP programmer.

ARDUINO_SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sh_common.inc.sh
source "${ARDUINO_SCRIPTS_DIR}/sh_common.inc.sh"

PROJECT_ROOT="$(cd "${ARDUINO_SCRIPTS_DIR}/../.." && pwd)"
FQBN_DEFAULT="arduino:avr:uno"
SKETCH_DEFAULT="encoder_controller"
SKETCH_NAME=""
PORT=""
FQBN="${FQBN_DEFAULT}"
PROGRAMMER=""
RESET_GPIO="4"
RESET_LOW_MS="120"
NO_RESET=0

usage() {
  cat <<'USAGE'
Usage: bash arduino/scripts/upload_avr_sketch.sh [<sketch-folder>] [<serial-port|usb>] [<fqbn>] [options]

With no arguments: uploads sketch "encoder_controller" using the first suitable USB serial port.

With ICSP (--programmer): if port is omitted, defaults to "usb" (typical for USB ISP tools).

Options:
  --programmer <id>   ICSP programmer id (e.g. usbtinyisp, usbasp, stk500v2)
  --reset-gpio <num>  Pulse this BCM GPIO LOW before upload (default: 4, Pi hosting target)
  --reset-low-ms <ms> Hold reset low duration (default: 120)
  --no-reset          Skip GPIO reset pulse

Examples:
  bash arduino/scripts/upload_avr_sketch.sh
  bash arduino/scripts/upload_avr_sketch.sh encoder_controller
  bash arduino/scripts/upload_avr_sketch.sh encoder_controller /dev/cu.usbserial-1120
  bash arduino/scripts/upload_avr_sketch.sh encoder_controller usb arduino:avr:uno --programmer usbtinyisp

Programmer ids match avrdude / arduino-cli; see: arduino-cli upload --help
USAGE
}

POSITIONAL=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --programmer)
      PROGRAMMER="${2:-}"
      shift 2
      ;;
    --reset-gpio)
      RESET_GPIO="${2:-}"
      shift 2
      ;;
    --reset-low-ms)
      RESET_LOW_MS="${2:-}"
      shift 2
      ;;
    --no-reset)
      NO_RESET=1
      shift
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
      POSITIONAL+=("$1")
      shift
      ;;
  esac
done

case "${#POSITIONAL[@]}" in
  0)
    SKETCH_NAME="${SKETCH_DEFAULT}"
    ;;
  1)
    SKETCH_NAME="${POSITIONAL[0]}"
    ;;
  2)
    SKETCH_NAME="${POSITIONAL[0]}"
    PORT="${POSITIONAL[1]}"
    ;;
  3)
    SKETCH_NAME="${POSITIONAL[0]}"
    PORT="${POSITIONAL[1]}"
    FQBN="${POSITIONAL[2]}"
    ;;
  *)
    echo "Too many positional arguments." >&2
    usage >&2
    exit 2
    ;;
esac

if [[ -z "${PORT}" ]]; then
  if [[ -n "${PROGRAMMER}" ]]; then
    PORT="usb"
  else
    PORT="$(elc_detect_serial_port avr)"
  fi
fi

SKETCH_DIR="${PROJECT_ROOT}/arduino/sketches/${SKETCH_NAME}"
BUILD_DIR="${PROJECT_ROOT}/arduino/build/${SKETCH_NAME}"

if ! [[ -d "${SKETCH_DIR}" ]]; then
  echo "Sketch directory not found: ${SKETCH_DIR}" >&2
  exit 1
fi

if ! [[ "${RESET_GPIO}" =~ ^[0-9]+$ ]]; then
  echo "Invalid --reset-gpio value: ${RESET_GPIO}" >&2
  exit 2
fi

if ! [[ "${RESET_LOW_MS}" =~ ^[0-9]+$ ]]; then
  echo "Invalid --reset-low-ms value: ${RESET_LOW_MS}" >&2
  exit 2
fi

pulse_reset_gpio() {
  local gpio="$1"
  local low_ms="$2"

  if command -v pinctrl >/dev/null 2>&1; then
    pinctrl set "${gpio}" op dh
    pinctrl set "${gpio}" dl
    python3 - "$low_ms" <<'PY'
import sys
import time
time.sleep(int(sys.argv[1]) / 1000.0)
PY
    pinctrl set "${gpio}" dh
    return 0
  fi

  if command -v raspi-gpio >/dev/null 2>&1; then
    raspi-gpio set "${gpio}" op dh
    raspi-gpio set "${gpio}" dl
    python3 - "$low_ms" <<'PY'
import sys
import time
time.sleep(int(sys.argv[1]) / 1000.0)
PY
    raspi-gpio set "${gpio}" dh
    return 0
  fi

  if command -v python3 >/dev/null 2>&1; then
    if python3 - "${gpio}" "${low_ms}" <<'PY'
import sys
import time
try:
    import RPi.GPIO as GPIO
except Exception:
    raise SystemExit(2)

pin = int(sys.argv[1])
low_ms = int(sys.argv[2])
GPIO.setwarnings(False)
GPIO.setmode(GPIO.BCM)
GPIO.setup(pin, GPIO.OUT, initial=GPIO.HIGH)
GPIO.output(pin, GPIO.LOW)
time.sleep(low_ms / 1000.0)
GPIO.output(pin, GPIO.HIGH)
time.sleep(0.05)
GPIO.cleanup(pin)
PY
    then
      return 0
    fi
  fi

  return 1
}

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "arduino-cli not found. Run: bash arduino/scripts/install_arduino_cli.sh" >&2
  exit 1
fi

elc_arduino_core_ensure "${FQBN}"

echo "Compiling ${SKETCH_DIR} (${FQBN})..."
mkdir -p "${BUILD_DIR}"
arduino-cli compile --fqbn "${FQBN}" --build-path "${BUILD_DIR}" "${SKETCH_DIR}"

if [[ "${NO_RESET}" -eq 0 && -z "${PROGRAMMER}" ]]; then
  echo "Pulsing GPIO ${RESET_GPIO} LOW->HIGH for target reset..."
  if ! pulse_reset_gpio "${RESET_GPIO}" "${RESET_LOW_MS}"; then
    echo "WARNING: Could not pulse GPIO reset; continuing upload." >&2
  fi
fi

echo "Uploading to ${PORT} (${FQBN})..."
upload_cmd=(arduino-cli upload --fqbn "${FQBN}" --port "${PORT}" --input-dir "${BUILD_DIR}")
if [[ -n "${PROGRAMMER}" ]]; then
  upload_cmd+=(--programmer "${PROGRAMMER}")
fi
upload_cmd+=("${SKETCH_DIR}")
"${upload_cmd[@]}"

echo "AVR upload complete."
