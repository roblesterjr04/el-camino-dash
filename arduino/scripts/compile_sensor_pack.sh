#!/usr/bin/env bash
set -euo pipefail

ARDUINO_SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sh_common.inc.sh
source "${ARDUINO_SCRIPTS_DIR}/sh_common.inc.sh"

PROJECT_ROOT="$(cd "${ARDUINO_SCRIPTS_DIR}/../.." && pwd)"
SKETCH_DIR="${PROJECT_ROOT}/arduino/sketches/sensor_pack"
BUILD_DIR="${PROJECT_ROOT}/arduino/build/sensor_pack"
FQBN="${1:-arduino:avr:uno}"

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "arduino-cli not found. Run: bash arduino/scripts/install_arduino_cli.sh" >&2
  exit 1
fi

elc_arduino_core_ensure "${FQBN}"

mkdir -p "${BUILD_DIR}"

echo "Compiling sensor_pack for ${FQBN}"
arduino-cli compile \
  --fqbn "${FQBN}" \
  --build-path "${BUILD_DIR}" \
  "${SKETCH_DIR}"

echo "Build output: ${BUILD_DIR}"
