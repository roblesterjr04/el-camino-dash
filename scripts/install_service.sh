#!/usr/bin/env bash
set -euo pipefail

DRY_RUN=0
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=1
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE_FILE="/etc/systemd/system/el-camino-dash.service"
RUN_USER="${SUDO_USER:-${USER}}"

run_cmd() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[dry-run] $*"
  else
    eval "$@"
  fi
}

if [[ "${EUID}" -ne 0 ]]; then
  echo "This script must run as root (use sudo)." >&2
  exit 1
fi

SERVICE_CONTENT="[Unit]
Description=El Camino Dashboard
After=network.target sound.target

[Service]
Type=simple
User=${RUN_USER}
WorkingDirectory=${PROJECT_DIR}
Environment=PYTHONPATH=${PROJECT_DIR}/src
ExecStart=${PROJECT_DIR}/.venv/bin/python ${PROJECT_DIR}/src/main.py --config ${PROJECT_DIR}/config/dashboard.yaml
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[dry-run] would write ${SERVICE_FILE}"
else
  printf "%s\n" "${SERVICE_CONTENT}" > "${SERVICE_FILE}"
fi

run_cmd "systemctl daemon-reload"
run_cmd "systemctl enable el-camino-dash.service"
run_cmd "systemctl restart el-camino-dash.service"
run_cmd "systemctl status --no-pager el-camino-dash.service || true"
