#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/deploy_to_pi.sh <user@host> [remote_dir] [options]

Examples:
  bash scripts/deploy_to_pi.sh pi@192.168.1.50
  bash scripts/deploy_to_pi.sh pi@elcamino.local /opt/el-camino-dash --with-pip
  bash scripts/deploy_to_pi.sh pi@elcamino.local --with-pip --install-services --logs

Options:
  --with-pip          Refresh Python deps in remote venv after sync.
  --install-services  Install/update tracked systemd units on remote target.
  --no-restart        Skip service restart after deploy.
  --logs              Print recent logs from gauge-cluster-hdmi service.
  -h, --help          Show this help message.

Notes:
  - Default remote_dir is /opt/el-camino-dash.
  - Sync is staged into remote user cache, then promoted via sudo rsync.
  - This script is intended for daily iterative updates on already-provisioned Pi images.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ $# -lt 1 ]]; then
  usage >&2
  exit 1
fi

TARGET="$1"
shift

REMOTE_DIR="/opt/el-camino-dash"
if [[ "${1:-}" == /* || "${1:-}" == "~" || "${1:-}" == ~/* ]]; then
  REMOTE_DIR="$1"
  shift
fi

WITH_PIP=0
INSTALL_SERVICES=0
RESTART_SERVICES=1
SHOW_LOGS=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --with-pip)
      WITH_PIP=1
      ;;
    --install-services)
      INSTALL_SERVICES=1
      ;;
    --no-restart)
      RESTART_SERVICES=0
      ;;
    --logs)
      SHOW_LOGS=1
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
  shift
done

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

for cmd in rsync ssh; do
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "${cmd} is required but not found." >&2
    exit 1
  fi
done

REMOTE_HOME="$(ssh "${TARGET}" 'printf "%s" "$HOME"')"
if [[ "${REMOTE_DIR}" == "~" ]]; then
  REMOTE_DIR="${REMOTE_HOME}"
elif [[ "${REMOTE_DIR}" == ~/* ]]; then
  REMOTE_DIR="${REMOTE_HOME}/${REMOTE_DIR#~/}"
fi
REMOTE_STAGE="${REMOTE_HOME}/.cache/el-camino-dash-deploy"

echo "Source: ${PROJECT_DIR}"
echo "Target: ${TARGET}:${REMOTE_DIR}"
echo "Stage:  ${REMOTE_STAGE}"

ssh "${TARGET}" "mkdir -p '${REMOTE_STAGE}'"

echo "Syncing runtime payload to remote stage..."
rsync -az --checksum --delete --itemize-changes \
  --exclude=".git/" \
  --exclude=".venv/" \
  --exclude=".cursor/" \
  --exclude="__pycache__/" \
  --exclude="*.pyc" \
  --exclude="arduino/build/" \
  --exclude=".DS_Store" \
  --exclude="data/*.dev.json" \
  "${PROJECT_DIR}/src/" "${TARGET}:${REMOTE_STAGE}/src/"
rsync -az --checksum --delete --itemize-changes "${PROJECT_DIR}/config/" "${TARGET}:${REMOTE_STAGE}/config/"
rsync -az --checksum --delete --itemize-changes "${PROJECT_DIR}/themes/" "${TARGET}:${REMOTE_STAGE}/themes/"
rsync -az --checksum --delete --itemize-changes "${PROJECT_DIR}/assets/" "${TARGET}:${REMOTE_STAGE}/assets/"
rsync -az --checksum --delete --itemize-changes "${PROJECT_DIR}/scripts/" "${TARGET}:${REMOTE_STAGE}/scripts/"
rsync -az --checksum --itemize-changes "${PROJECT_DIR}/requirements.txt" "${TARGET}:${REMOTE_STAGE}/requirements.txt"
rsync -az --checksum --delete --itemize-changes "${PROJECT_DIR}/deploy/" "${TARGET}:${REMOTE_STAGE}/deploy/"

echo "Promoting staged payload to ${REMOTE_DIR} (sudo rsync)..."
ssh "${TARGET}" "sudo mkdir -p '${REMOTE_DIR}' && sudo rsync -a --delete '${REMOTE_STAGE}/' '${REMOTE_DIR}/' && sudo chown -R pi:pi '${REMOTE_DIR}'"

echo "Ensuring executable bits on remote scripts..."
ssh "${TARGET}" "sudo find '${REMOTE_DIR}/scripts' -type f -name '*.sh' -exec chmod +x {} +"
ssh "${TARGET}" "sudo chmod +x '${REMOTE_DIR}/scripts/gauge-mcp-monitor'"

if [[ "${WITH_PIP}" -eq 1 ]]; then
  echo "Refreshing remote virtualenv dependencies..."
  ssh "${TARGET}" "sudo -u pi bash -lc 'python3 -m venv --system-site-packages \"${REMOTE_DIR}/.venv\" && \"${REMOTE_DIR}/.venv/bin/python\" -m pip install -r \"${REMOTE_DIR}/requirements.txt\"'"
fi

if [[ "${INSTALL_SERVICES}" -eq 1 ]]; then
  echo "Installing/updating remote systemd units..."
  ssh "${TARGET}" "sudo bash '${REMOTE_DIR}/scripts/install_runtime_services.sh'"
fi

if [[ "${RESTART_SERVICES}" -eq 1 ]]; then
  echo "Restarting runtime services..."
  ssh "${TARGET}" "sudo systemctl restart gauge-ir-protocol.service || true"
  ssh "${TARGET}" "sudo systemctl restart gauge-cluster-hdmi.service || true"
  ssh "${TARGET}" "sudo systemctl restart gauge-hide-cursor.service || true"
  ssh "${TARGET}" "sudo systemctl restart gauge-power-control.service || true"
fi

if [[ "${SHOW_LOGS}" -eq 1 ]]; then
  echo "Recent HDMI service logs:"
  ssh "${TARGET}" "sudo journalctl -u gauge-cluster-hdmi.service -n 80 --no-pager"
fi

echo "Deploy complete."
