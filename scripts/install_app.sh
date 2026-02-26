#!/usr/bin/env bash
set -euo pipefail

DRY_RUN=0
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=1
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="${PROJECT_DIR}/.venv"

run_cmd() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[dry-run] $*"
  else
    eval "$@"
  fi
}

run_cmd "python3 -m venv '${VENV_DIR}'"
run_cmd "'${VENV_DIR}/bin/pip' install --upgrade pip"
run_cmd "'${VENV_DIR}/bin/pip' install -r '${PROJECT_DIR}/requirements.txt'"
run_cmd "PYTHONPATH='${PROJECT_DIR}/src' '${VENV_DIR}/bin/python' -m py_compile '${PROJECT_DIR}/src/main.py'"
run_cmd "PYTHONPATH='${PROJECT_DIR}/src' '${VENV_DIR}/bin/python' -c \"from dashboard.config import load_dashboard_config; load_dashboard_config('${PROJECT_DIR}/config/dashboard.dev.yaml'); print('Config validation passed')\""
