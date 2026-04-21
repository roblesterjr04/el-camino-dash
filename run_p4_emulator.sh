#!/usr/bin/env bash
# run_p4_emulator.sh — launch the ESP32-P4 round gauge emulator
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_BIN="${PROJECT_DIR}/.venv/bin/python"

if [[ ! -x "${VENV_BIN}" ]]; then
  echo "No .venv found at ${PROJECT_DIR}/.venv — run scripts/run_sim.sh once to create it."
  exit 1
fi

cd "${PROJECT_DIR}"
exec "${VENV_BIN}" "${PROJECT_DIR}/p4_emulator.py" "$@"
