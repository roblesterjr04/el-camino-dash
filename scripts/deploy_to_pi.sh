#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/deploy_to_pi.sh <user@host> [remote_dir] [options]

Examples:
  bash scripts/deploy_to_pi.sh pi@192.168.1.50
  bash scripts/deploy_to_pi.sh pi@elcamino.local /opt/el-camino-dash --with-pip
  bash scripts/deploy_to_pi.sh pi@elcamino.local --with-pip --install-services --boot-hdmi-rotation --logs

Options:
  --with-pip          Refresh Python deps in remote venv after sync.
  --install-services  Copy deploy/systemd/*.service into /etc/systemd/system and daemon-reload.
  --boot-hdmi-rotation  Add display_hdmi_rotate=2 under each ELC [HDMI:n] block in config.txt (reboot to apply).
  --no-restart        Skip service restart after deploy.
  --logs              Print recent logs from HDMI gauge services.
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
BOOT_HDMI_ROTATION=0
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
    --boot-hdmi-rotation)
      BOOT_HDMI_ROTATION=1
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
# Preserve runtime-managed virtualenv and data artifacts across iterative deploys.
ssh "${TARGET}" "sudo mkdir -p '${REMOTE_DIR}' && sudo rsync -a --delete --exclude '.venv/' --exclude 'data/' '${REMOTE_STAGE}/' '${REMOTE_DIR}/' && sudo chown -R pi:pi '${REMOTE_DIR}'"

echo "Ensuring executable bits on remote scripts..."
ssh "${TARGET}" "sudo find '${REMOTE_DIR}/scripts' -type f -name '*.sh' -exec chmod +x {} +"
ssh "${TARGET}" "sudo chmod +x '${REMOTE_DIR}/scripts/gauge-mcp-monitor'"

if [[ "${INSTALL_SERVICES}" -eq 1 ]]; then
  echo "Installing systemd units from ${REMOTE_DIR}/deploy/systemd/..."
  ssh "${TARGET}" "sudo bash -lc 'shopt -s nullglob; for u in \"${REMOTE_DIR}\"/deploy/systemd/*.service; do install -m644 \"\$u\" /etc/systemd/system/; done; systemctl daemon-reload'"
fi

if [[ "${BOOT_HDMI_ROTATION}" -eq 1 ]]; then
  echo "Patching firmware config.txt for display_hdmi_rotate=2 (reboot required)..."
  ssh "${TARGET}" bash -s <<'EOS'
set -euo pipefail
CFG=""
for d in /boot/firmware /boot; do
  if [[ -f "${d}/config.txt" ]]; then CFG="${d}/config.txt"; break; fi
done
if [[ -z "${CFG}" ]]; then echo "[elc-deploy] No config.txt under /boot; skipping rotation patch." >&2; exit 0; fi
if ! grep -q '# ELC-HDMI-BEGIN' "${CFG}"; then echo "[elc-deploy] No ELC HDMI block in config.txt; skipping." >&2; exit 0; fi
if grep -q '^display_hdmi_rotate=2' "${CFG}"; then echo "[elc-deploy] display_hdmi_rotate=2 already present." >&2; exit 0; fi
sudo cp -a "${CFG}" "${CFG}.elc-bak-$(date +%Y%m%d%H%M%S)"
export CFG
sudo env CFG="${CFG}" python3 <<'PY'
from pathlib import Path
import os

cfg = Path(os.environ["CFG"])
text = cfg.read_text()
lines = text.splitlines(keepends=True)
out = []
in_elc = False
i = 0
while i < len(lines):
    line = lines[i]
    if "# ELC-HDMI-BEGIN" in line:
        in_elc = True
    if "# ELC-HDMI-END" in line:
        in_elc = False
    out.append(line)
    if in_elc and line.strip().startswith("hdmi_cvt="):
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        if "display_hdmi_rotate" not in nxt:
            out.append("display_hdmi_rotate=2\n")
    i += 1
cfg.write_text("".join(out))
PY
EOS
fi

if [[ "${WITH_PIP}" -eq 1 ]]; then
  echo "Refreshing remote virtualenv dependencies..."
  ssh "${TARGET}" "sudo -u pi bash -lc 'python3 -m venv --system-site-packages \"${REMOTE_DIR}/.venv\" && \"${REMOTE_DIR}/.venv/bin/python\" -m pip install -r \"${REMOTE_DIR}/requirements.txt\"'"
fi

if [[ "${RESTART_SERVICES}" -eq 1 ]]; then
  echo "Restarting runtime services..."
  ssh "${TARGET}" "sudo systemctl try-restart gauge-cluster-hdmi.service gauge-cluster-hdmi-x11-extended.service || true"
  ssh "${TARGET}" "sudo systemctl restart gauge-ir-protocol.service || true"
  ssh "${TARGET}" "sudo systemctl restart gauge-power-control.service || true"
fi

if [[ "${SHOW_LOGS}" -eq 1 ]]; then
  echo "Recent HDMI service logs:"
  ssh "${TARGET}" "sudo journalctl -u gauge-cluster-hdmi.service -u gauge-cluster-hdmi-x11-extended.service -n 80 --no-pager"
fi

echo "Deploy complete."
