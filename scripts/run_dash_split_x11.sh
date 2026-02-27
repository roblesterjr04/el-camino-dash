#!/usr/bin/env bash
set -euo pipefail

# Starts a dedicated X11 session for the dashboard while keeping tty boot flow.
# Intended usage from tty:
#   bash scripts/run_dash_split_x11.sh
#
# Optional overrides:
#   DASH_X11_DISPLAY=":1"
#   DASH_X11_VT="8"
#   DASH_X11_PRIMARY_OUTPUT="Virtual-2"
#   DASH_X11_DISABLE_OUTPUT="Virtual-1"
#   DASH_CONFIG_PATH="/mnt/shared/config/dashboard.vm.yaml"

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_PATH="${DASH_CONFIG_PATH:-${PROJECT_DIR}/config/dashboard.vm.yaml}"
X_DISPLAY="${DASH_X11_DISPLAY:-:1}"
X_VT="${DASH_X11_VT:-8}"
PRIMARY_OUTPUT="${DASH_X11_PRIMARY_OUTPUT:-Virtual-2}"
DISABLE_OUTPUT="${DASH_X11_DISABLE_OUTPUT:-Virtual-1}"
LOG_DIR="${HOME}/.cache/el-camino-dash"
LOG_FILE="${LOG_DIR}/split-x11.log"

mkdir -p "${LOG_DIR}"

if ! command -v xinit >/dev/null 2>&1; then
  echo "xinit is required. Install with: sudo apt-get install -y xinit xorg x11-xserver-utils" >&2
  exit 1
fi

if [[ ! -x "${PROJECT_DIR}/scripts/run_hw.sh" ]]; then
  echo "Missing runner: ${PROJECT_DIR}/scripts/run_hw.sh" >&2
  exit 1
fi

if [[ ! -f "${CONFIG_PATH}" ]]; then
  echo "Config file not found: ${CONFIG_PATH}" >&2
  exit 1
fi

echo "Launching dashboard X session on ${X_DISPLAY}"
echo "Primary output: ${PRIMARY_OUTPUT}, disable output: ${DISABLE_OUTPUT}"
echo "Logs: ${LOG_FILE}"

exec xinit /bin/bash -lc "
  set -euo pipefail
  sleep 1
  if command -v xrandr >/dev/null 2>&1; then
    xrandr --output \"${PRIMARY_OUTPUT}\" --auto --primary --output \"${DISABLE_OUTPUT}\" --off || true
  fi
  cd \"${PROJECT_DIR}\"
  exec \"${PROJECT_DIR}/scripts/run_hw.sh\" \"${CONFIG_PATH}\"
" -- "${X_DISPLAY}" -nolisten tcp >>"${LOG_FILE}" 2>&1
