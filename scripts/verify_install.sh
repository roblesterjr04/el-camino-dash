#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_PY="${PROJECT_DIR}/.venv/bin/python"
PASS_COUNT=0
FAIL_COUNT=0

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

if [[ -x "${VENV_PY}" ]]; then
  pass "Virtualenv python found"
else
  fail "Virtualenv python missing; run scripts/install_app.sh"
fi

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

if [[ -x "${VENV_PY}" ]]; then
  if PYTHONPATH="${PROJECT_DIR}/src" "${VENV_PY}" -c "from dashboard.config import load_dashboard_config; load_dashboard_config('${PROJECT_DIR}/config/dashboard.yaml')" >/dev/null 2>&1; then
    pass "Primary config validates"
  else
    fail "Primary config validation failed"
  fi
fi

if [[ "${FAIL_COUNT}" -gt 0 ]]; then
  echo "Verification result: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
  exit 1
fi

echo "Verification result: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
