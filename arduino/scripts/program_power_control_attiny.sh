#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKETCH_PATH="${PROJECT_ROOT}/arduino/sketches/power_control/power_control.ino"
BUILD_DIR="${PROJECT_ROOT}/arduino/build/power_control_attiny"

MCU="attiny85"
PROGRAMMER="linuxspi"
PROGRAMMER_PORT=""
PROGRAMMER_BAUD=""
SPI_DEV="/dev/spidev0.0"
GPIO_CHIP="/dev/gpiochip0"
RESET_GPIO="4"
F_CPU_HZ="8000000"
WRITE_FUSES=0
LFUSE="0xE2"
HFUSE="0xDF"
EFUSE="0xFF"
RUN_VERIFY=1
QUIET=0
DEBUG=0
COMPILE_TIMEOUT_S="120"
AVRDUDE_TIMEOUT_S="30"
BITCLOCK=""

usage() {
  cat <<'USAGE'
Usage: bash arduino/scripts/program_power_control_attiny.sh [options]

Compiles and flashes arduino/sketches/power_control/power_control.ino
to an ATtiny over ISP using avrdude.

Options:
  --mcu <name>           MCU (default: attiny85)
  --programmer <name>    avrdude programmer id (default: linuxspi)
  --port <path>          Programmer serial port (required for some programmers)
  --baud <value>         Programmer baud (for serial programmers)
  --spi-dev <path>       SPI dev for linuxspi (default: /dev/spidev0.0)
  --gpiochip <path>      GPIO chip for linuxspi (default: /dev/gpiochip0)
  --reset-gpio <num>     Reset GPIO for linuxspi (default: 4)
  --f-cpu <hz>           Compile-time F_CPU define (default: 8000000)
  --set-fuses            Also write fuses for 8MHz internal clock
  --lfuse <hex>          Low fuse byte (default: 0xE2)
  --hfuse <hex>          High fuse byte (default: 0xDF)
  --efuse <hex>          Extended fuse byte (default: 0xFF)
  --no-verify            Skip avrdude verify pass
  --quiet                Reduce avrdude output verbosity
  --debug                Enable shell trace output (set -x)
  --compile-timeout <s>  Compile timeout seconds (default: 120)
  --avrdude-timeout <s>  Per-avrdude-step timeout seconds (default: 30)
  --bitclock <period>    avrdude bit clock period (e.g. 100, 10, 5us)
  --wiring               Print ISP + target wiring and exit
  -h, --help             Show this help

Examples:
  # Raspberry Pi as ISP via SPI0 (default)
  bash arduino/scripts/program_power_control_attiny.sh

  # Raspberry Pi as ISP with explicit SPI0 + reset pin
  bash arduino/scripts/program_power_control_attiny.sh \
    --programmer linuxspi --spi-dev /dev/spidev0.0 --reset-gpio 4

  # Slower SPI clock for marginal wiring/targets
  bash arduino/scripts/program_power_control_attiny.sh --bitclock 100

  # USBasp
  bash arduino/scripts/program_power_control_attiny.sh --programmer usbasp

  # Arduino-as-ISP (Uno on /dev/ttyACM0)
  bash arduino/scripts/program_power_control_attiny.sh \
    --programmer arduino --port /dev/ttyACM0 --baud 19200

Raspberry Pi SPI0 wiring (ATtiny85 DIP-8 target):
  Pi GPIO10 (pin 19, MOSI) -> ATtiny pin 5 (PB0/MOSI)
  Pi GPIO9  (pin 21, MISO) -> ATtiny pin 6 (PB1/MISO)
  Pi GPIO11 (pin 23, SCLK) -> ATtiny pin 7 (PB2/SCK)
  Pi GPIO4 (pin 7, RESET)  -> ATtiny pin 1 (RESET)
  Pi 3V3 (pin 1)           -> ATtiny pin 8 (VCC)
  Pi GND (pin 6)           -> ATtiny pin 4 (GND)

Generic ISP wiring (non-Pi programmers):
  Programmer MOSI -> ATtiny pin 5 (PB0/MOSI)
  Programmer MISO -> ATtiny pin 6 (PB1/MISO)
  Programmer SCK  -> ATtiny pin 7 (PB2/SCK)
  Programmer RESET-> ATtiny pin 1 (RESET)
  Programmer VCC  -> ATtiny pin 8 (VCC)
  Programmer GND  -> ATtiny pin 4 (GND)

Power-control target pins in this project:
  Pin 5 (PB0)  : PI_POWER   -> 2N3904 base -> U3 ON/OFF
  Pin 6 (PB1)  : PI_SIGNAL  -> Raspberry Pi GPIO16 (shutdown notice)
  Pin 7 (PB2)  : IGN_SENSE  <- divider from IGN 12V
  Pin 2 (PB3)  : PI_CONFIRM <- Raspberry Pi GPIO12 (shutdown complete)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mcu)
      MCU="${2:-}"
      shift 2
      ;;
    --programmer)
      PROGRAMMER="${2:-}"
      shift 2
      ;;
    --port)
      PROGRAMMER_PORT="${2:-}"
      shift 2
      ;;
    --baud)
      PROGRAMMER_BAUD="${2:-}"
      shift 2
      ;;
    --spi-dev)
      SPI_DEV="${2:-}"
      shift 2
      ;;
    --gpiochip)
      GPIO_CHIP="${2:-}"
      shift 2
      ;;
    --reset-gpio)
      RESET_GPIO="${2:-}"
      shift 2
      ;;
    --f-cpu)
      F_CPU_HZ="${2:-}"
      shift 2
      ;;
    --set-fuses)
      WRITE_FUSES=1
      shift
      ;;
    --lfuse)
      LFUSE="${2:-}"
      shift 2
      ;;
    --hfuse)
      HFUSE="${2:-}"
      shift 2
      ;;
    --efuse)
      EFUSE="${2:-}"
      shift 2
      ;;
    --no-verify)
      RUN_VERIFY=0
      shift
      ;;
    --quiet)
      QUIET=1
      shift
      ;;
    --debug)
      DEBUG=1
      shift
      ;;
    --compile-timeout)
      COMPILE_TIMEOUT_S="${2:-}"
      shift 2
      ;;
    --avrdude-timeout)
      AVRDUDE_TIMEOUT_S="${2:-}"
      shift 2
      ;;
    --bitclock)
      BITCLOCK="${2:-}"
      shift 2
      ;;
    --wiring)
      usage
      exit 0
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "${DEBUG}" -eq 1 ]]; then
  set -x
fi

for tool in avr-g++ avr-objcopy avrdude; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "Missing required tool: ${tool}" >&2
    echo "Install avr toolchain + avrdude first." >&2
    exit 1
  fi
done

mkdir -p "${BUILD_DIR}"
ELF_PATH="${BUILD_DIR}/power_control.elf"
HEX_PATH="${BUILD_DIR}/power_control.hex"
COMPILE_LOG="${BUILD_DIR}/compile.log"

echo "Compiling ${SKETCH_PATH} for ${MCU} (F_CPU=${F_CPU_HZ})..."
echo "Compiler log: ${COMPILE_LOG}"
set +e
if command -v timeout >/dev/null 2>&1; then
  timeout "${COMPILE_TIMEOUT_S}" \
    avr-g++ \
      -x c++ \
      -std=gnu++11 \
      -Wall \
      -Wextra \
      -Os \
      -mmcu="${MCU}" \
      -DF_CPU="${F_CPU_HZ}UL" \
      "${SKETCH_PATH}" \
      -o "${ELF_PATH}" 2>&1 | tee "${COMPILE_LOG}"
  COMPILE_STATUS=${PIPESTATUS[0]}
else
  avr-g++ \
    -x c++ \
    -std=gnu++11 \
    -Wall \
    -Wextra \
    -Os \
    -mmcu="${MCU}" \
    -DF_CPU="${F_CPU_HZ}UL" \
    "${SKETCH_PATH}" \
    -o "${ELF_PATH}" 2>&1 | tee "${COMPILE_LOG}"
  COMPILE_STATUS=${PIPESTATUS[0]}
fi
set -e
if [[ "${COMPILE_STATUS}" -eq 124 ]]; then
  echo "ERROR: compile timed out after ${COMPILE_TIMEOUT_S}s." >&2
  echo "Re-run with --debug or increase timeout via --compile-timeout <seconds>." >&2
  exit 1
fi
if [[ "${COMPILE_STATUS}" -ne 0 ]]; then
  echo "ERROR: compile failed (status ${COMPILE_STATUS})." >&2
  echo "See compiler log: ${COMPILE_LOG}" >&2
  exit 1
fi
echo "Compile complete."

avr-objcopy -O ihex -R .eeprom "${ELF_PATH}" "${HEX_PATH}"
echo "HEX generated: ${HEX_PATH}"

AVRDUDE_ARGS=(-p "${MCU}" -c "${PROGRAMMER}")

if [[ "${PROGRAMMER}" == "linuxspi" ]]; then
  if [[ ! -e "${SPI_DEV}" ]]; then
    echo "SPI device not found: ${SPI_DEV}" >&2
    echo "Enable SPI0 and ensure spidev node exists." >&2
    exit 1
  fi
  # avrdude 7.1 supports reset pin in linuxspi -P argument:
  #   <spidev>:<gpiochip>:<reset-gpio>
  AVRDUDE_ARGS=(-p "${MCU}" -c "linuxspi")
  if [[ -e "${GPIO_CHIP}" ]]; then
    AVRDUDE_ARGS+=(-P "${SPI_DEV}:${GPIO_CHIP}:${RESET_GPIO}")
  else
    AVRDUDE_ARGS+=(-P "${SPI_DEV}::${RESET_GPIO}")
  fi
elif [[ -n "${PROGRAMMER_PORT}" ]]; then
  AVRDUDE_ARGS+=(-P "${PROGRAMMER_PORT}")
fi
if [[ -n "${PROGRAMMER_BAUD}" ]]; then
  AVRDUDE_ARGS+=(-b "${PROGRAMMER_BAUD}")
fi
if [[ -n "${BITCLOCK}" ]]; then
  AVRDUDE_ARGS+=(-B "${BITCLOCK}")
fi

if [[ "${QUIET}" -eq 0 ]]; then
  AVRDUDE_ARGS+=(-v)
fi

run_avrdude_stage() {
  local stage="$1"
  shift
  echo "[avrdude] ${stage}..."
  if command -v timeout >/dev/null 2>&1; then
    timeout "${AVRDUDE_TIMEOUT_S}" avrdude "${AVRDUDE_ARGS[@]}" "$@"
    local status=$?
    if [[ "${status}" -ne 0 ]]; then
      if [[ "${status}" -eq 124 ]]; then
        echo "ERROR: avrdude timed out during '${stage}' after ${AVRDUDE_TIMEOUT_S}s." >&2
      else
        echo "ERROR: avrdude failed during '${stage}' (exit ${status})." >&2
      fi
      return "${status}"
    fi
  else
    avrdude "${AVRDUDE_ARGS[@]}" "$@"
    local status=$?
    if [[ "${status}" -ne 0 ]]; then
      echo "ERROR: avrdude failed during '${stage}' (exit ${status})." >&2
      return "${status}"
    fi
  fi
}

echo "Programmer: ${PROGRAMMER}"
echo "MCU: ${MCU}"
if [[ "${PROGRAMMER}" == "linuxspi" ]]; then
  echo "SPI dev: ${SPI_DEV}"
  echo "Reset GPIO: ${RESET_GPIO}"
  if [[ -z "${BITCLOCK}" ]]; then
    echo "Bitclock: 100 (auto default for linuxspi)"
    AVRDUDE_ARGS+=(-B "100")
  fi
elif [[ -n "${BITCLOCK}" ]]; then
  echo "Bitclock: ${BITCLOCK}"
fi

SIG_FILE="$(mktemp)"
trap 'rm -f "${SIG_FILE}"' EXIT
run_avrdude_stage "probe-signature" -U "signature:r:${SIG_FILE}:r" || {
  echo "ERROR: Could not read target signature." >&2
  echo "Check ISP wiring, power, and reset pin." >&2
  exit 1
}
SIG_HEX="$(od -An -tx1 "${SIG_FILE}" | tr -s ' ' | tr '\n' ' ' | sed -E 's/^ //; s/ $//')"
echo "Target signature: ${SIG_HEX:-unknown}"

if [[ "${WRITE_FUSES}" -eq 1 ]]; then
  echo "Writing fuses: lfuse=${LFUSE} hfuse=${HFUSE} efuse=${EFUSE}"
  run_avrdude_stage "write-fuses" \
    -U "lfuse:w:${LFUSE}:m" \
    -U "hfuse:w:${HFUSE}:m" \
    -U "efuse:w:${EFUSE}:m"
fi

echo "Flashing ${HEX_PATH}..."
run_avrdude_stage "flash" -V -U "flash:w:${HEX_PATH}:i"

if [[ "${RUN_VERIFY}" -eq 1 ]]; then
  echo "Verifying flash..."
  run_avrdude_stage "verify" -U "flash:v:${HEX_PATH}:i"
fi

echo "ATtiny programming complete."
echo "Build output: ${BUILD_DIR}"
