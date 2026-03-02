#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo bash scripts/install_early_boot_splash_service.sh" >&2
  exit 1
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_USER="${SUDO_USER:-${USER}}"
PYTHON_BIN=""

SERVICE_FILE="/etc/systemd/system/gauge-boot-splash.service"
IMAGE_PATH="${PROJECT_DIR}/assets/bootscreen/boot.png"
SCRIPT_PATH="${PROJECT_DIR}/scripts/early_boot_splash.py"

if [[ ! -f "${IMAGE_PATH}" ]]; then
  echo "Missing image: ${IMAGE_PATH}" >&2
  exit 1
fi
if [[ ! -f "${SCRIPT_PATH}" ]]; then
  echo "Missing script: ${SCRIPT_PATH}" >&2
  exit 1
fi

echo "Installing Python dependencies for early splash..."
apt-get update
apt-get install -y python3-pil python3-spidev python3-rpi.gpio

VENV_PY="${PROJECT_DIR}/.venv/bin/python"
if [[ -x "${VENV_PY}" ]] && sudo -u "${RUN_USER}" "${VENV_PY}" -c "import PIL, spidev, RPi.GPIO" >/dev/null 2>&1; then
  PYTHON_BIN="${VENV_PY}"
elif /usr/bin/python3 -c "import PIL, spidev, RPi.GPIO" >/dev/null 2>&1; then
  PYTHON_BIN="/usr/bin/python3"
else
  echo "No suitable Python interpreter found with PIL/spidev/RPi.GPIO." >&2
  echo "Try running: sudo apt-get install -y python3-pil python3-spidev python3-rpi.gpio" >&2
  exit 1
fi

cat > "${SERVICE_FILE}" <<EOF
[Unit]
Description=Early SPI Boot Splash (3x GC9A01)
DefaultDependencies=no
After=local-fs.target
Before=sysinit.target shutdown.target
Conflicts=shutdown.target

[Service]
Type=oneshot
User=${RUN_USER}
WorkingDirectory=${PROJECT_DIR}
ExecStart=${PYTHON_BIN} ${SCRIPT_PATH} --image ${IMAGE_PATH} --speed-hz 8000000 --madctl 0x08 --colmod 0x55
RemainAfterExit=yes
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=sysinit.target
EOF

systemctl daemon-reload
systemctl enable gauge-boot-splash.service

echo "Installed and enabled: gauge-boot-splash.service"
echo "Using Python: ${PYTHON_BIN}"
echo "Start now (optional): sudo systemctl start gauge-boot-splash.service"
echo "Check logs: sudo journalctl -u gauge-boot-splash.service -b"
