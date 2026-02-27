#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_PY="${PROJECT_DIR}/.venv/bin/python"
TARGET="pi"
DISPLAY_MODE="${DISPLAY_MODE:-hardware}"
PASS_COUNT=0
FAIL_COUNT=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target=*)
      TARGET="${1#*=}"
      shift
      ;;
    --target)
      TARGET="${2:-pi}"
      shift 2
      ;;
    --display-mode=*)
      DISPLAY_MODE="${1#*=}"
      shift
      ;;
    --display-mode)
      DISPLAY_MODE="${2:-hardware}"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

TARGET="$(printf "%s" "${TARGET}" | tr '[:upper:]' '[:lower:]')"
DISPLAY_MODE="$(printf "%s" "${DISPLAY_MODE}" | tr '[:upper:]' '[:lower:]')"

pass() {
  echo "[PASS] $1"
  PASS_COUNT=$((PASS_COUNT + 1))
}

fail() {
  echo "[FAIL] $1"
  FAIL_COUNT=$((FAIL_COUNT + 1))
}

check_file() {
  if [[ -f "$1" ]]; then
    pass "File exists: $1"
  else
    fail "Missing file: $1"
  fi
}

check_file "${PROJECT_DIR}/config/dashboard.yaml"
check_file "${PROJECT_DIR}/config/dashboard.dev.yaml"
check_file "${PROJECT_DIR}/themes/default/theme.json"
check_file "${PROJECT_DIR}/src/main.py"

if [[ "${TARGET}" == "pi" || "${TARGET}" == "vm" ]]; then
  pass "Verification target: ${TARGET}"
else
  fail "Unsupported --target value: ${TARGET} (expected pi or vm)"
fi

if [[ "${DISPLAY_MODE}" == "auto" || "${DISPLAY_MODE}" == "xvfb" || "${DISPLAY_MODE}" == "hardware" ]]; then
  pass "Display mode: ${DISPLAY_MODE}"
else
  fail "Unsupported --display-mode value: ${DISPLAY_MODE}"
fi

if [[ -x "${VENV_PY}" ]]; then
  pass "Virtualenv python found"
else
  fail "Virtualenv python missing; run scripts/install_app.sh"
fi

if [[ "${TARGET}" == "pi" ]]; then
  if [[ -c /dev/spidev0.0 || -c /dev/spidev0.1 ]]; then
    pass "SPI device present"
  else
    fail "SPI device not found (check dtparam=spi=on and reboot)"
  fi

  if command -v raspi-gpio >/dev/null 2>&1; then
    pass "GPIO tooling available"
  else
    fail "raspi-gpio missing"
  fi
else
  pass "Skipping SPI/GPIO checks for VM target"
fi

if [[ "${TARGET}" == "vm" ]]; then
  if command -v Xvfb >/dev/null 2>&1; then
    pass "Xvfb binary available"
  else
    fail "Xvfb not found (install with apt-get install -y xvfb)"
  fi
fi

if [[ -x "${VENV_PY}" ]]; then
  if PYTHONPATH="${PROJECT_DIR}/src" "${VENV_PY}" -c "from dashboard.config import load_dashboard_config; load_dashboard_config('${PROJECT_DIR}/config/dashboard.yaml')" >/dev/null 2>&1; then
    pass "Primary config validates"
  else
    fail "Primary config validation failed"
  fi
fi

if [[ -x "${VENV_PY}" ]]; then
  DISPLAY_FOR_TEST="${DISPLAY:-}"
  if [[ "${DISPLAY_MODE}" == "xvfb" && -z "${DISPLAY_FOR_TEST}" ]]; then
    DISPLAY_FOR_TEST=":99"
  fi
  if DISPLAY_MODE="${DISPLAY_MODE}" DISPLAY="${DISPLAY_FOR_TEST}" PYTHONPATH="${PROJECT_DIR}/src" "${VENV_PY}" -c "import pygame; pygame.display.init(); pygame.display.set_mode((64, 64)); pygame.display.quit()" >/dev/null 2>&1; then
    pass "Pygame display smoke test passed (${DISPLAY_MODE})"
  else
    fail "Pygame display smoke test failed (${DISPLAY_MODE}); check DISPLAY/Xvfb setup"
  fi
fi

if [[ "${FAIL_COUNT}" -gt 0 ]]; then
  echo "Verification result: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
  exit 1
fi

echo "Verification result: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
