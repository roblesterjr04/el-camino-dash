#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKETCH_DIR="${PROJECT_ROOT}/arduino/sketches/sensor_pack"
BUILD_DIR="${PROJECT_ROOT}/arduino/build/sensor_pack"
FQBN="${1:-arduino:avr:uno}"

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "arduino-cli not found. Run: bash arduino/scripts/install_arduino_cli.sh" >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}"

echo "Compiling sensor_pack for ${FQBN}"
arduino-cli compile \
  --fqbn "${FQBN}" \
  --build-path "${BUILD_DIR}" \
  "${SKETCH_DIR}"

echo "Build output: ${BUILD_DIR}"
