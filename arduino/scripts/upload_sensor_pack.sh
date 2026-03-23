#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKETCH_DIR="${PROJECT_ROOT}/arduino/sketches/sensor_pack"
BUILD_DIR="${PROJECT_ROOT}/arduino/build/sensor_pack"
PORT=""
FQBN="arduino:avr:uno"
PROGRAMMER=""
RESET_GPIO="4"
RESET_LOW_MS="120"
NO_RESET=0
SSH_TARGET=""
SSH_PORT=""
REMOTE_DEFAULT_PORT="/dev/serial0"
KEEP_REMOTE=0
REMOTE_BOOTSTRAP=1

usage() {
  cat <<'USAGE'
Usage: bash arduino/scripts/upload_sensor_pack.sh [serial-port] [fqbn] [options]

Compiles and uploads arduino/sketches/sensor_pack/sensor_pack.ino.
By default, pulses GPIO4 LOW->HIGH before upload to reset the Arduino.

Options:
  --ssh-target <user@host> Run compile/upload on remote Pi over SSH.
  --ssh-port <num>         SSH TCP port for remote mode.
  --programmer <id>      Upload via programmer (example: usbtinyisp)
  --reset-gpio <num>     GPIO number to pulse for reset (default: 4)
  --reset-low-ms <ms>    Time to hold reset LOW in ms (default: 120)
  --no-reset             Skip GPIO reset pulse
  --no-remote-bootstrap  Do not auto-install arduino-cli/core on remote host
  --keep-remote          Keep remote temp build directory (SSH mode debug)
  -h, --help             Show this help

Examples:
  # Local USB upload from this host:
  bash arduino/scripts/upload_sensor_pack.sh /dev/cu.usbmodem101 arduino:avr:uno

  # Upload from Pi side over SSH (uses default /dev/serial0 on Pi):
  bash arduino/scripts/upload_sensor_pack.sh --ssh-target pi@digidash.local

  # Upload from Pi side over SSH using explicit serial path:
  bash arduino/scripts/upload_sensor_pack.sh /dev/serial0
  bash arduino/scripts/upload_sensor_pack.sh /dev/serial0 arduino:avr:uno --ssh-target pi@digidash.local

  # Upload using ISP programmer (USBtinyISP):
  bash arduino/scripts/upload_sensor_pack.sh usb arduino:avr:uno --programmer usbtinyisp
USAGE
}

if [[ $# -eq 0 ]]; then
  usage >&2
  exit 2
fi

POSITIONAL=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --ssh-target)
      SSH_TARGET="${2:-}"
      shift 2
      ;;
    --ssh-port)
      SSH_PORT="${2:-}"
      shift 2
      ;;
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
    --keep-remote)
      KEEP_REMOTE=1
      shift
      ;;
    --no-remote-bootstrap)
      REMOTE_BOOTSTRAP=0
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

if [[ ${#POSITIONAL[@]} -ge 1 ]]; then
  PORT="${POSITIONAL[0]}"
fi
if [[ ${#POSITIONAL[@]} -ge 2 ]]; then
  FQBN="${POSITIONAL[1]}"
fi

if [[ -n "${SSH_TARGET}" && -z "${PORT}" ]]; then
  PORT="${REMOTE_DEFAULT_PORT}"
fi

if [[ -z "${PORT}" ]]; then
  usage >&2
  exit 2
fi

if ! [[ "${RESET_GPIO}" =~ ^[0-9]+$ ]]; then
  echo "Invalid --reset-gpio value: ${RESET_GPIO}" >&2
  exit 2
fi

if ! [[ "${RESET_LOW_MS}" =~ ^[0-9]+$ ]]; then
  echo "Invalid --reset-low-ms value: ${RESET_LOW_MS}" >&2
  exit 2
fi

if [[ -n "${SSH_PORT}" ]] && ! [[ "${SSH_PORT}" =~ ^[0-9]+$ ]]; then
  echo "Invalid --ssh-port value: ${SSH_PORT}" >&2
  exit 2
fi

pulse_reset_gpio() {
  local gpio="$1"
  local low_ms="$2"

  # Preferred on modern Raspberry Pi OS images.
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

  # Common on legacy Raspberry Pi OS images.
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

  # Fallback if RPi.GPIO is present.
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

run_local_upload() {
  if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "arduino-cli not found. Run: bash arduino/scripts/install_arduino_cli.sh" >&2
    exit 1
  fi

  echo "Compiling before upload..."
  mkdir -p "${BUILD_DIR}"
  arduino-cli compile \
    --fqbn "${FQBN}" \
    --build-path "${BUILD_DIR}" \
    "${SKETCH_DIR}"

  if [[ "${NO_RESET}" -eq 0 ]]; then
    echo "Pulsing GPIO ${RESET_GPIO} LOW->HIGH for Arduino reset..."
    if ! pulse_reset_gpio "${RESET_GPIO}" "${RESET_LOW_MS}"; then
      echo "WARNING: Could not pulse GPIO reset (need pinctrl, raspi-gpio, or python3+RPi.GPIO)." >&2
      echo "         Continuing upload without explicit GPIO reset." >&2
    fi
  fi

  echo "Uploading to ${PORT} (${FQBN})"
  local upload_cmd=(arduino-cli upload --fqbn "${FQBN}" --port "${PORT}" --input-dir "${BUILD_DIR}")
  if [[ -n "${PROGRAMMER}" ]]; then
    upload_cmd+=(--programmer "${PROGRAMMER}")
  fi
  upload_cmd+=("${SKETCH_DIR}")
  "${upload_cmd[@]}"

  echo "Upload complete."
}

run_remote_upload() {
  local ssh_cmd=(ssh)
  if [[ -n "${SSH_PORT}" ]]; then
    ssh_cmd+=(-p "${SSH_PORT}")
  fi
  ssh_cmd+=("${SSH_TARGET}")

  for tool in ssh tar; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
      echo "Missing required tool for SSH mode: ${tool}" >&2
      exit 1
    fi
  done

  local remote_tmp="/tmp/elc-sensor-pack-upload-${USER:-user}-$$"
  local status=0

  echo "Preparing remote workspace on ${SSH_TARGET}:${remote_tmp}..."
  "${ssh_cmd[@]}" "rm -rf '${remote_tmp}' && mkdir -p '${remote_tmp}'"

  echo "Copying sketch to remote Pi..."
  tar -C "${PROJECT_ROOT}" -cf - "arduino/sketches/sensor_pack" | "${ssh_cmd[@]}" "tar -xf - -C '${remote_tmp}'"

  echo "Running compile/upload on remote Pi ${SSH_TARGET} (port ${PORT}, fqbn ${FQBN})..."
  set +e
  "${ssh_cmd[@]}" bash -s -- "${remote_tmp}" "${PORT}" "${FQBN}" "${PROGRAMMER}" "${RESET_GPIO}" "${RESET_LOW_MS}" "${NO_RESET}" "${REMOTE_BOOTSTRAP}" <<'EOF'
set -euo pipefail
remote_tmp="$1"
port="$2"
fqbn="$3"
programmer="$4"
reset_gpio="$5"
reset_low_ms="$6"
no_reset="$7"
remote_bootstrap="$8"
sketch_dir="${remote_tmp}/arduino/sketches/sensor_pack"
build_dir="${remote_tmp}/arduino/build/sensor_pack"

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

ensure_remote_arduino_cli() {
  if command -v arduino-cli >/dev/null 2>&1; then
    return 0
  fi

  if [[ "${remote_bootstrap}" -eq 0 ]]; then
    echo "arduino-cli not found on remote host." >&2
    echo "Install on Pi first (or remove --no-remote-bootstrap)." >&2
    return 1
  fi

  echo "arduino-cli not found on remote host; attempting bootstrap install..."

  if command -v apt-get >/dev/null 2>&1; then
    # Prefer distro package first on Raspberry Pi OS/Debian.
    sudo apt-get update >/dev/null 2>&1 || true
    sudo apt-get install -y arduino-cli >/dev/null 2>&1 || true
  fi

  if ! command -v arduino-cli >/dev/null 2>&1; then
    if ! command -v curl >/dev/null 2>&1; then
      echo "curl is required to install arduino-cli via upstream installer." >&2
      return 1
    fi
    mkdir -p "${HOME}/.local/bin"
    curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh \
      | BINDIR="${HOME}/.local/bin" sh
    export PATH="${HOME}/.local/bin:${PATH}"
  fi

  if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "Failed to install arduino-cli on remote host." >&2
    return 1
  fi

  return 0
}

ensure_remote_core() {
  # Example: arduino:avr:uno -> arduino:avr
  local required_core
  required_core="$(printf "%s" "${fqbn}" | awk -F: '{print $1 ":" $2}')"

  arduino-cli core update-index
  if ! arduino-cli core list | awk -v core="${required_core}" '$1 == core {found=1} END {exit found ? 0 : 1}'; then
    echo "Installing Arduino core: ${required_core}"
    arduino-cli core install "${required_core}"
  fi
}

ensure_remote_arduino_cli
ensure_remote_core

mkdir -p "${build_dir}"
arduino-cli compile --fqbn "${fqbn}" --build-path "${build_dir}" "${sketch_dir}"

if [[ "${no_reset}" -eq 0 ]]; then
  echo "Pulsing GPIO ${reset_gpio} LOW->HIGH for Arduino reset..."
  if ! pulse_reset_gpio "${reset_gpio}" "${reset_low_ms}"; then
    echo "WARNING: Could not pulse GPIO reset on remote host; continuing upload." >&2
  fi
fi

upload_cmd=(arduino-cli upload --fqbn "${fqbn}" --port "${port}" --input-dir "${build_dir}")
if [[ -n "${programmer}" ]]; then
  upload_cmd+=(--programmer "${programmer}")
fi
upload_cmd+=("${sketch_dir}")
"${upload_cmd[@]}"
EOF
  status=$?
  set -e

  if [[ "${KEEP_REMOTE}" -eq 0 ]]; then
    "${ssh_cmd[@]}" "rm -rf '${remote_tmp}'" || true
  else
    echo "Keeping remote temp dir for debugging: ${remote_tmp}"
  fi

  if [[ ${status} -ne 0 ]]; then
    exit ${status}
  fi

  echo "Remote upload complete."
}

if [[ -n "${SSH_TARGET}" ]]; then
  run_remote_upload
else
  run_local_upload
fi
