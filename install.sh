#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo bash install.sh" >&2
  exit 1
fi

TARGET_USER="${SUDO_USER:-${USER}}"
TARGET_HOME="$(getent passwd "${TARGET_USER}" | cut -d: -f6)"
if [[ -z "${TARGET_HOME}" || ! -d "${TARGET_HOME}" ]]; then
  echo "Unable to resolve home directory for user: ${TARGET_USER}" >&2
  exit 1
fi

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${TARGET_HOME}/gauge-cluster"
VENV_DIR="${PROJECT_DIR}/.venv"
SERVICE_FILE="/etc/systemd/system/gauge-cluster.service"

if [[ -f /boot/firmware/config.txt ]]; then
  BOOT_DIR="/boot/firmware"
elif [[ -f /boot/config.txt ]]; then
  BOOT_DIR="/boot"
else
  echo "Could not find config.txt under /boot/firmware or /boot" >&2
  exit 1
fi

CONFIG_TXT="${BOOT_DIR}/config.txt"
CMDLINE_TXT="${BOOT_DIR}/cmdline.txt"
BOOT_CHANGED=0

append_line_if_missing() {
  local file="$1"
  local line="$2"
  if grep -Fqx "${line}" "${file}"; then
    return 0
  fi
  printf "\n%s\n" "${line}" >> "${file}"
  BOOT_CHANGED=1
}

ensure_cmdline_token() {
  local file="$1"
  local token="$2"
  local current
  current="$(<"${file}")"
  if [[ "${current}" == *"${token}"* ]]; then
    return 0
  fi
  printf "%s %s\n" "${current}" "${token}" > "${file}"
  BOOT_CHANGED=1
}

write_if_changed() {
  local file="$1"
  local content="$2"
  if [[ -f "${file}" ]] && diff -q <(printf "%s\n" "${content}") "${file}" >/dev/null; then
    return 0
  fi
  printf "%s\n" "${content}" > "${file}"
}

# echo "[1/7] Installing apt packages..."
# apt-get update
# apt-get install -y \
#   python3-pip python3-venv python3-dev python3-pygame python3-pil python3-spidev python3-rpi.gpio \
#   libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libsdl2-ttf-dev libfreetype6-dev \
#   git cmake

# echo "[2/7] Configuring boot files in ${BOOT_DIR}..."
# append_line_if_missing "${CONFIG_TXT}" "dtparam=spi=on"
# append_line_if_missing "${CONFIG_TXT}" "dtoverlay=spi1-3cs"
# ensure_cmdline_token "${CMDLINE_TXT}" "spidev.bufsiz=65536"

# echo "[3/7] Syncing project into ${PROJECT_DIR}..."
# install -d -o "${TARGET_USER}" -g "${TARGET_USER}" "${PROJECT_DIR}"
# cp -a "${SOURCE_DIR}/." "${PROJECT_DIR}/"
# chown -R "${TARGET_USER}:${TARGET_USER}" "${PROJECT_DIR}"

echo "[4/7] Creating virtual environment..."
if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
  sudo -u "${TARGET_USER}" python3 -m venv --system-site-packages "${VENV_DIR}"
fi

echo "[5/7] Installing Python packages into venv..."
sudo -u "${TARGET_USER}" "${VENV_DIR}/bin/python" -m pip install --upgrade pip
sudo -u "${TARGET_USER}" "${VENV_DIR}/bin/python" -m pip install spidev RPi.GPIO Pillow pygame numpy

echo "[6/7] Installing systemd service..."
SERVICE_CONTENT="[Unit]
Description=Gauge Cluster Dashboard
After=multi-user.target

[Service]
Type=simple
User=${TARGET_USER}
WorkingDirectory=${PROJECT_DIR}
Environment=SDL_VIDEODRIVER=dummy
Environment=HEADLESS=1
Environment=PYTHONPATH=${PROJECT_DIR}/src
ExecStart=${VENV_DIR}/bin/python ${PROJECT_DIR}/src/main.py --config ${PROJECT_DIR}/config/dashboard.spi.yaml --render-mode spi --headless
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
"
write_if_changed "${SERVICE_FILE}" "${SERVICE_CONTENT}"
systemctl daemon-reload
systemctl enable gauge-cluster.service

echo "[7/7] Installation complete."
echo
echo "Project directory: ${PROJECT_DIR}"
echo "Manual start:"
echo "  cd ${PROJECT_DIR} && HEADLESS=1 SDL_VIDEODRIVER=dummy PYTHONPATH=src .venv/bin/python src/main.py --config config/dashboard.spi.yaml --render-mode spi --headless"
echo "Service controls:"
echo "  sudo systemctl start gauge-cluster.service"
echo "  sudo systemctl status gauge-cluster.service"
echo "  sudo journalctl -u gauge-cluster.service -f"

if [[ "${BOOT_CHANGED}" -eq 1 ]]; then
  echo
  echo "Boot configuration changed. Reboot required:"
  echo "  sudo reboot"
fi
