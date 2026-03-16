#!/usr/bin/env bash
set -euo pipefail

RUNTIME_DIR="/opt/el-camino-dash"
CONFIG_PATH="/opt/el-camino-dash/config/dashboard.hdmi5.yaml"
PASS_COUNT=0
FAIL_COUNT=0

usage() {
  cat <<'EOF'
Usage:
  bash scripts/verify_install.sh [options]

Options:
  --runtime-dir <path>   Runtime directory to validate (default: /opt/el-camino-dash)
  --config <path>        Config file to validate (default: /opt/el-camino-dash/config/dashboard.hdmi5.yaml)
  -h, --help             Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --runtime-dir)
      RUNTIME_DIR="${2:-}"
      shift 2
      ;;
    --runtime-dir=*)
      RUNTIME_DIR="${1#*=}"
      shift
      ;;
    --config)
      CONFIG_PATH="${2:-}"
      shift 2
      ;;
    --config=*)
      CONFIG_PATH="${1#*=}"
      shift
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

check_dir() {
  if [[ -d "$1" ]]; then
    pass "Directory exists: $1"
  else
    fail "Missing directory: $1"
  fi
}

VENV_PY="${RUNTIME_DIR}/.venv/bin/python"

check_dir "${RUNTIME_DIR}"
check_dir "${RUNTIME_DIR}/src"
check_dir "${RUNTIME_DIR}/config"
check_dir "${RUNTIME_DIR}/themes"
check_dir "${RUNTIME_DIR}/assets"
check_file "${RUNTIME_DIR}/requirements.txt"
check_file "${CONFIG_PATH}"

if [[ -x "${VENV_PY}" ]]; then
  pass "Virtualenv python found: ${VENV_PY}"
else
  fail "Virtualenv python missing: ${VENV_PY}"
fi

if command -v systemctl >/dev/null 2>&1; then
  for svc in gauge-cluster-hdmi.service gauge-hide-cursor.service gauge-power-control.service gauge-ir-protocol.service; do
    if systemctl is-enabled "${svc}" >/dev/null 2>&1; then
      pass "Service enabled: ${svc}"
    else
      fail "Service not enabled: ${svc}"
    fi
  done
else
  fail "systemctl not available"
fi

if command -v ir-keytable >/dev/null 2>&1; then
  pass "IR tooling available (ir-keytable)"
else
  fail "ir-keytable missing"
fi

if [[ -x "${VENV_PY}" ]]; then
  if PYTHONPATH="${RUNTIME_DIR}/src" "${VENV_PY}" -c "from dashboard.config import load_dashboard_config; load_dashboard_config('${CONFIG_PATH}')" >/dev/null 2>&1; then
    pass "Runtime config validates"
  else
    fail "Runtime config validation failed"
  fi
fi

if [[ -f /boot/firmware/config.txt || -f /boot/config.txt ]]; then
  boot_cfg="/boot/firmware/config.txt"
  [[ -f "${boot_cfg}" ]] || boot_cfg="/boot/config.txt"
  if rg -q "^dtparam=i2c_arm=on$" "${boot_cfg}"; then
    pass "Boot I2C enabled"
  else
    fail "Boot I2C not enabled"
  fi
  if rg -q "^dtoverlay=vc4-kms-v3d$" "${boot_cfg}"; then
    pass "Boot KMS enabled"
  else
    fail "Boot KMS not enabled"
  fi
fi

echo "Verification result: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
if [[ "${FAIL_COUNT}" -gt 0 ]]; then
  exit 1
fi
