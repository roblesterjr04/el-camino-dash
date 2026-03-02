#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/deploy_to_pi.sh <user@host> [remote_dir]

Examples:
  bash scripts/deploy_to_pi.sh pi@192.168.1.50
  bash scripts/deploy_to_pi.sh pi@elcamino.local /home/pi/gauge-cluster

Notes:
  - Uses rsync over SSH to mirror files to the Pi.
  - Default remote_dir is $HOME/gauge-cluster for the remote user.
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
REMOTE_DIR="${2:-}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v rsync >/dev/null 2>&1; then
  echo "rsync is required but not found." >&2
  exit 1
fi

if ! command -v ssh >/dev/null 2>&1; then
  echo "ssh is required but not found." >&2
  exit 1
fi

if [[ -z "${REMOTE_DIR}" ]]; then
  REMOTE_HOME="$(ssh "${TARGET}" 'printf "%s" "$HOME"')"
  REMOTE_DIR="${REMOTE_HOME}/gauge-cluster"
fi

echo "Ensuring remote directory exists: ${REMOTE_DIR}"
ssh "${TARGET}" "mkdir -p '${REMOTE_DIR}'"

echo "Mirroring project to ${TARGET}:${REMOTE_DIR} (with deletions)..."
rsync -az --delete \
  --exclude=".git/" \
  --exclude=".venv/" \
  --exclude=".cursor/" \
  --exclude="__pycache__/" \
  --exclude="*.pyc" \
  --exclude=".DS_Store" \
  "${PROJECT_DIR}/" "${TARGET}:${REMOTE_DIR}/"

echo "Ensuring executable bit on all remote .sh files..."
ssh "${TARGET}" "bash -lc 'shopt -s globstar nullglob; for script in \"${REMOTE_DIR}\"/**/*.sh; do chmod +x \"\$script\"; done'"

echo "Deploy complete."
echo "Remote path: ${REMOTE_DIR}"
