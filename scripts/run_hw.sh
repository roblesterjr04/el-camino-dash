#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_PATH="${1:-${PROJECT_DIR}/config/dashboard.vehicle.yaml}"

echo "Hardware run config: ${CONFIG_PATH}"
echo "DISPLAY=${DISPLAY:-<unset>} SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-<unset>}"

if [[ -n "${SDL_VIDEODRIVER:-}" ]]; then
  exec env DISPLAY_MODE="hardware" SDL_VIDEODRIVER="${SDL_VIDEODRIVER}" "${PROJECT_DIR}/scripts/run_sim.sh" "${CONFIG_PATH}"
fi

exec env DISPLAY_MODE="hardware" "${PROJECT_DIR}/scripts/run_sim.sh" "${CONFIG_PATH}"
