#!/usr/bin/env bash
set -euo pipefail

SKIP_PACKAGES=0
SKIP_SYNC=0
MODE="spi"
BOOT_SPLASH="auto"

usage() {
  cat <<'USAGE'
Usage: sudo bash install.sh [options]

Options:
  --skip-packages        Skip apt-get update/install
  --skip-sync            Skip rsync copy to ~/gauge-cluster
  --mode <value>         spi | hdmi | both (default: spi)
  --boot-splash <value>  auto | none | spi | hdmi | both (default: auto)
  -h, --help             Show help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-packages)
      SKIP_PACKAGES=1
      shift
      ;;
    --skip-sync)
      SKIP_SYNC=1
      shift
      ;;
    --mode=*)
      MODE="${1#*=}"
      shift
      ;;
    --mode)
      MODE="${2:-}"
      if [[ -z "${MODE}" ]]; then
        echo "Missing value for --mode" >&2
        exit 2
      fi
      shift 2
      ;;
    --boot-splash=*)
      BOOT_SPLASH="${1#*=}"
      shift
      ;;
    --boot-splash)
      BOOT_SPLASH="${2:-}"
      if [[ -z "${BOOT_SPLASH}" ]]; then
        echo "Missing value for --boot-splash" >&2
        exit 2
      fi
      shift 2
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

MODE="$(printf "%s" "${MODE}" | tr '[:upper:]' '[:lower:]')"
if [[ "${MODE}" != "spi" && "${MODE}" != "hdmi" && "${MODE}" != "both" ]]; then
  echo "Invalid --mode: ${MODE}. Expected spi, hdmi, both." >&2
  exit 2
fi

BOOT_SPLASH="$(printf "%s" "${BOOT_SPLASH}" | tr '[:upper:]' '[:lower:]')"
if [[ "${BOOT_SPLASH}" != "auto" && "${BOOT_SPLASH}" != "none" && "${BOOT_SPLASH}" != "spi" && "${BOOT_SPLASH}" != "hdmi" && "${BOOT_SPLASH}" != "both" ]]; then
  echo "Invalid --boot-splash: ${BOOT_SPLASH}. Expected auto, none, spi, hdmi, both." >&2
  exit 2
fi
if [[ "${BOOT_SPLASH}" == "auto" ]]; then
  BOOT_SPLASH="${MODE}"
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo bash install.sh" >&2
  exit 1
fi

echo "[0/9] Pre-cleaning apt caches..."
apt-get clean || true
apt-get autoclean -y || true
rm -rf /var/lib/apt/lists/* || true
rm -f /var/cache/apt/archives/*.deb || true

TARGET_USER="${SUDO_USER:-${USER}}"
TARGET_HOME="$(getent passwd "${TARGET_USER}" | cut -d: -f6)"
if [[ -z "${TARGET_HOME}" || ! -d "${TARGET_HOME}" ]]; then
  echo "Unable to resolve home directory for user: ${TARGET_USER}" >&2
  exit 1
fi

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${TARGET_HOME}/gauge-cluster"
VENV_DIR="${PROJECT_DIR}/.venv"

SPI_SERVICE_FILE="/etc/systemd/system/gauge-cluster-spi.service"
HDMI_SERVICE_FILE="/etc/systemd/system/gauge-cluster-hdmi.service"
CURSOR_SERVICE_FILE="/etc/systemd/system/gauge-hide-cursor.service"
LEGACY_SERVICE_FILE="/etc/systemd/system/gauge-cluster.service"

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

append_line_if_missing() {
  local file="$1"
  local line="$2"
  if ! awk -v target="$line" '$0 == target {found=1} END {exit found?0:1}' "$file"; then
    printf "\n%s\n" "$line" >> "$file"
  fi
}

ensure_cmdline_token() {
  local file="$1"
  local token="$2"
  local current
  current="$(<"${file}")"
  if [[ "${current}" != *"${token}"* ]]; then
    printf "%s %s\n" "${current}" "${token}" > "${file}"
  fi
}

echo "[1/9] Installing system packages..."
if [[ "${SKIP_PACKAGES}" -eq 1 ]]; then
  echo "Skipping package install (--skip-packages)"
else
  apt-get update
  apt-get install -y \
    python3-pip python3-venv python3-dev \
    python3-pygame python3-pil python3-spidev python3-rpi.gpio \
    libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libsdl2-ttf-dev libfreetype-dev \
    git cmake rsync x11-utils \
    bluez pulseaudio pulseaudio-module-bluetooth \
    navit gpsd gpsd-clients \
    plymouth plymouth-themes unclutter
  systemctl enable bluetooth.service >/dev/null 2>&1 || true
  systemctl restart bluetooth.service >/dev/null 2>&1 || true
fi

echo "[2/9] Configuring boot settings..."
if [[ "${MODE}" == "spi" || "${MODE}" == "both" || "${BOOT_SPLASH}" == "spi" || "${BOOT_SPLASH}" == "both" ]]; then
  append_line_if_missing "${CONFIG_TXT}" "dtparam=spi=on"
  append_line_if_missing "${CONFIG_TXT}" "dtoverlay=spi1-3cs"
  ensure_cmdline_token "${CMDLINE_TXT}" "spidev.bufsiz=65536"
fi

if [[ "${MODE}" == "hdmi" || "${MODE}" == "both" || "${BOOT_SPLASH}" == "hdmi" || "${BOOT_SPLASH}" == "both" ]]; then
  append_line_if_missing "${CONFIG_TXT}" "hdmi_force_hotplug=1"
  append_line_if_missing "${CONFIG_TXT}" "hdmi_group=2"
  append_line_if_missing "${CONFIG_TXT}" "hdmi_mode=87"
  append_line_if_missing "${CONFIG_TXT}" "hdmi_cvt=800 480 60 6 0 0 0"
  append_line_if_missing "${CONFIG_TXT}" "disable_overscan=1"
fi

echo "[3/9] Syncing project..."
if [[ "${SKIP_SYNC}" -eq 1 ]]; then
  echo "Skipping sync (--skip-sync)"
  if [[ ! -d "${PROJECT_DIR}" ]]; then
    echo "Missing ${PROJECT_DIR}; run once without --skip-sync." >&2
    exit 1
  fi
else
  install -d -o "${TARGET_USER}" -g "${TARGET_USER}" "${PROJECT_DIR}"
  rsync -a --delete \
    --exclude ".git/" \
    --exclude ".venv/" \
    --exclude "__pycache__/" \
    "${SOURCE_DIR}/" "${PROJECT_DIR}/"
  chown -R "${TARGET_USER}:${TARGET_USER}" "${PROJECT_DIR}"
fi

echo "[4/9] Building Python environment..."
if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
  sudo -u "${TARGET_USER}" python3 -m venv --system-site-packages "${VENV_DIR}"
fi
sudo -u "${TARGET_USER}" "${VENV_DIR}/bin/python" -m pip install --upgrade pip
if [[ -f "${PROJECT_DIR}/requirements.txt" ]]; then
  sudo -u "${TARGET_USER}" "${VENV_DIR}/bin/python" -m pip install -r "${PROJECT_DIR}/requirements.txt"
fi
sudo -u "${TARGET_USER}" "${VENV_DIR}/bin/python" -m pip install spidev RPi.GPIO Pillow numpy

echo "[5/9] Writing services..."
cat > "${SPI_SERVICE_FILE}" <<SERVICESPI
[Unit]
Description=Gauge Cluster Dashboard (SPI)
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
SERVICESPI

cat > "${HDMI_SERVICE_FILE}" <<SERVICEHDMI
[Unit]
Description=Gauge Cluster Dashboard (HDMI)
After=graphical.target display-manager.service
Wants=graphical.target display-manager.service

[Service]
Type=simple
User=${TARGET_USER}
WorkingDirectory=${PROJECT_DIR}
Environment=DISPLAY=:0
Environment=XAUTHORITY=${TARGET_HOME}/.Xauthority
Environment=DISPLAY_MODE=hardware
Environment=PYTHONPATH=${PROJECT_DIR}/src
ExecStart=/bin/bash -lc 'export DISPLAY=:0; export XAUTHORITY=${TARGET_HOME}/.Xauthority; until [ -S /tmp/.X11-unix/X0 ]; do sleep 1; done; cd ${PROJECT_DIR}; exec bash scripts/run_hw.sh config/dashboard.hdmi5.yaml'
Restart=always
RestartSec=5

[Install]
WantedBy=graphical.target
WantedBy=multi-user.target
SERVICEHDMI

cat > "${CURSOR_SERVICE_FILE}" <<SERVICECURSOR
[Unit]
Description=Hide X11 Mouse Cursor
After=graphical.target display-manager.service
Wants=graphical.target display-manager.service

[Service]
Type=simple
User=${TARGET_USER}
Environment=DISPLAY=:0
Environment=XAUTHORITY=${TARGET_HOME}/.Xauthority
ExecStart=/bin/bash -lc 'until [ -S /tmp/.X11-unix/X0 ]; do sleep 1; done; exec /usr/bin/unclutter -display :0 -idle 0 -root -grab'
Restart=always
RestartSec=2

[Install]
WantedBy=graphical.target
WantedBy=multi-user.target
SERVICECURSOR

rm -f "${LEGACY_SERVICE_FILE}" || true
systemctl daemon-reload

echo "[6/9] Applying mode selection..."
systemctl disable gauge-cluster.service >/dev/null 2>&1 || true
systemctl stop gauge-cluster.service >/dev/null 2>&1 || true

case "${MODE}" in
  spi)
    systemctl unmask gauge-cluster-spi.service >/dev/null 2>&1 || true
    systemctl enable gauge-cluster-spi.service
    systemctl restart gauge-cluster-spi.service || true
    systemctl disable gauge-cluster-hdmi.service >/dev/null 2>&1 || true
    systemctl disable gauge-hide-cursor.service >/dev/null 2>&1 || true
    systemctl stop gauge-cluster-hdmi.service >/dev/null 2>&1 || true
    systemctl stop gauge-hide-cursor.service >/dev/null 2>&1 || true
    ;;
  hdmi)
    systemctl set-default graphical.target || true
    systemctl mask gauge-cluster-spi.service >/dev/null 2>&1 || true
    systemctl disable gauge-cluster-spi.service >/dev/null 2>&1 || true
    systemctl stop gauge-cluster-spi.service >/dev/null 2>&1 || true
    systemctl enable gauge-cluster-hdmi.service
    systemctl enable gauge-hide-cursor.service
    systemctl restart gauge-cluster-hdmi.service || true
    systemctl restart gauge-hide-cursor.service || true
    ;;
  both)
    systemctl set-default graphical.target || true
    systemctl unmask gauge-cluster-spi.service >/dev/null 2>&1 || true
    systemctl enable gauge-cluster-spi.service
    systemctl enable gauge-cluster-hdmi.service
    systemctl enable gauge-hide-cursor.service
    systemctl restart gauge-cluster-spi.service || true
    systemctl restart gauge-cluster-hdmi.service || true
    systemctl restart gauge-hide-cursor.service || true
    ;;
esac

echo "[7/9] Applying boot splash selection..."
if [[ "${BOOT_SPLASH}" == "spi" || "${BOOT_SPLASH}" == "both" ]]; then
  systemctl unmask gauge-boot-splash.service >/dev/null 2>&1 || true
  bash "${PROJECT_DIR}/scripts/install_early_boot_splash_service.sh"
else
  systemctl mask gauge-boot-splash.service >/dev/null 2>&1 || true
  systemctl disable gauge-boot-splash.service >/dev/null 2>&1 || true
  systemctl stop gauge-boot-splash.service >/dev/null 2>&1 || true
fi

if [[ "${BOOT_SPLASH}" == "hdmi" || "${BOOT_SPLASH}" == "both" ]]; then
  bash "${PROJECT_DIR}/scripts/setup_boot_image.sh"
fi

echo "[8/9] Verifying Navit + Bluetooth audio prerequisites..."
if command -v navit >/dev/null 2>&1; then
  echo "  Navit executable found."
else
  echo "  WARNING: navit command not found. Install package 'navit' or set integrations.navit.executable."
fi

if command -v bluetoothctl >/dev/null 2>&1; then
  echo "  bluetoothctl found."
else
  echo "  WARNING: bluetoothctl not found. Install package 'bluez'."
fi

if command -v pactl >/dev/null 2>&1; then
  echo "  pactl found."
else
  echo "  WARNING: pactl not found. Install package 'pulseaudio'."
fi

usermod -aG bluetooth "${TARGET_USER}" >/dev/null 2>&1 || true
usermod -aG audio "${TARGET_USER}" >/dev/null 2>&1 || true

echo "[9/9] Final apt cache cleanup..."
apt-get clean || true
apt-get autoclean -y || true
rm -rf /var/lib/apt/lists/* || true
rm -f /var/cache/apt/archives/*.deb || true

echo
echo "Fresh install complete."
echo "Mode: ${MODE}"
echo "Boot splash: ${BOOT_SPLASH}"
echo "Project: ${PROJECT_DIR}"
echo
echo "Navit/Bluetooth notes:"
echo "  - Enable integrations.navit.enabled and integrations.bluetooth_audio.enabled in config"
echo "  - Bluetooth pairing/connection uses bluetoothctl and PulseAudio sinks via pactl"
echo "  - Reboot recommended so audio/bluetooth group changes fully apply"
echo
echo "Quick checks:"
echo "  systemctl status gauge-cluster-hdmi.service -l --no-pager"
echo "  systemctl status gauge-cluster-spi.service -l --no-pager"
echo "  bluetoothctl show"
echo "  navit --version"
echo
echo "Reboot recommended:"
echo "  sudo reboot"
