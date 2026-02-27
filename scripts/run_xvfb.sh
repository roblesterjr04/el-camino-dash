#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_PATH="${1:-${PROJECT_DIR}/config/dashboard.vm.yaml}"
DISPLAY_ID="${DISPLAY:-:99}"
XVFB_SCREEN="${XVFB_SCREEN:-1920x1080x24}"
XVFB_LOG="${XVFB_LOG:-/tmp/el-camino-dash-xvfb.log}"

if ! touch "${XVFB_LOG}" >/dev/null 2>&1; then
  FALLBACK_LOG_DIR="${XDG_STATE_HOME:-${HOME}/.local/state}/el-camino-dash"
  mkdir -p "${FALLBACK_LOG_DIR}"
  XVFB_LOG="${FALLBACK_LOG_DIR}/xvfb.log"
fi

if ! command -v Xvfb >/dev/null 2>&1; then
  echo "Xvfb is not installed. Install it with: sudo apt-get install -y xvfb" >&2
  exit 1
fi

if ! pgrep -f "Xvfb ${DISPLAY_ID}" >/dev/null 2>&1; then
  echo "Starting Xvfb on ${DISPLAY_ID} (${XVFB_SCREEN})"
  Xvfb "${DISPLAY_ID}" -screen 0 "${XVFB_SCREEN}" >"${XVFB_LOG}" 2>&1 &
  sleep 1
fi

if ! pgrep -f "Xvfb ${DISPLAY_ID}" >/dev/null 2>&1; then
  echo "Failed to start Xvfb on ${DISPLAY_ID}. Check ${XVFB_LOG}" >&2
  exit 1
fi

echo "Using virtual display ${DISPLAY_ID}"
# Force SDL to use X11 under Xvfb (prevents fbcon fallback on some VMs).
exec env DISPLAY="${DISPLAY_ID}" DISPLAY_MODE="xvfb" SDL_VIDEODRIVER="x11" "${PROJECT_DIR}/scripts/run_sim.sh" "${CONFIG_PATH}"
