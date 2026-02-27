#!/usr/bin/env bash
set -euo pipefail

DRY_RUN=0
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=1
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_VENV_DIR="${PROJECT_DIR}/.venv"
if [[ "${PROJECT_DIR}" == /mnt/shared* ]]; then
  DEFAULT_VENV_DIR="${HOME}/.el-camino-dash/.venv"
fi
VENV_DIR="${EL_CAMINO_VENV_DIR:-${DEFAULT_VENV_DIR}}"

run_cmd() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[dry-run] $*"
  else
    eval "$@"
  fi
}

if [[ "$DRY_RUN" -ne 1 && -d "${VENV_DIR}" ]]; then
  if ! "${VENV_DIR}/bin/python" -c "import sys" >/dev/null 2>&1; then
    echo "Existing virtualenv is not runnable on this machine; rebuilding."
    rm -rf "${VENV_DIR}"
  fi
fi

run_cmd "mkdir -p '$(dirname "${VENV_DIR}")'"
run_cmd "python3 -m venv --copies '${VENV_DIR}'"
run_cmd "'${VENV_DIR}/bin/python' -m pip install --upgrade pip"
run_cmd "'${VENV_DIR}/bin/python' -m pip install -r '${PROJECT_DIR}/requirements.txt'"
run_cmd "PYTHONPATH='${PROJECT_DIR}/src' '${VENV_DIR}/bin/python' -m py_compile '${PROJECT_DIR}/src/main.py'"
run_cmd "PYTHONPATH='${PROJECT_DIR}/src' '${VENV_DIR}/bin/python' -c \"from dashboard.config import load_dashboard_config; load_dashboard_config('${PROJECT_DIR}/config/dashboard.dev.yaml'); print('Config validation passed')\""
