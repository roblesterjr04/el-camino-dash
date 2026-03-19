#!/usr/bin/env bash
set -euo pipefail

cat <<'EOF'
install.sh has been archived.

This project now uses an image-first workflow based on rpi-image-gen:

  1) Build a provision image:
     bash scripts/build_image.sh

  2) For day-to-day development updates on a provisioned Pi:
     bash scripts/deploy_to_pi.sh <user@host>

Legacy install logic has been removed from this entrypoint.
Use git history if you need to inspect the old monolithic installer.
EOF

exit 1
#!/usr/bin/env bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
export PIP_NO_CACHE_DIR=1
export PIP_DISABLE_PIP_VERSION_CHECK=1

APT_INSTALL_ARGS=(-y --no-install-recommends -o Dpkg::Use-Pty=0)

SKIP_PACKAGES=0
SKIP_SYNC=0
SKIP_ARDUINO=0
MODE="hdmi"
BOOT_SPLASH="auto"
ARDUINO_PORT="auto"
ARDUINO_FQBN="arduino:avr:uno"
ARDUINO_RESET_GPIO=4
ARDUINO_BAUD=115200
ARDUINO_FW_STATUS="skipped"
IR_GPIO_PIN=26

usage() {
  cat <<'USAGE'
Usage: sudo bash install.sh [options]

Options:
  --skip-packages        Skip apt-get update/install
  --skip-sync            Skip rsync copy to ~/gauge-cluster
  --skip-arduino         Skip Arduino firmware compile/upload over UART
  --mode <value>         hdmi (default); spi/both accepted but coerced to hdmi
  --boot-splash <value>  auto | none | hdmi (spi/both accepted but disabled)
  --arduino-port <path>  Serial port for upload (default: auto: /dev/serial0, then USB)
  --arduino-fqbn <value> Arduino FQBN (default: arduino:avr:uno)
  --arduino-reset-gpio   GPIO for ATmega reset pulse (default: 4)
  --arduino-baud         UART bootloader baud (default: 115200)
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
    --skip-arduino)
      SKIP_ARDUINO=1
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
    --arduino-port=*)
      ARDUINO_PORT="${1#*=}"
      shift
      ;;
    --arduino-port)
      ARDUINO_PORT="${2:-}"
      if [[ -z "${ARDUINO_PORT}" ]]; then
        echo "Missing value for --arduino-port" >&2
        exit 2
      fi
      shift 2
      ;;
    --arduino-fqbn=*)
      ARDUINO_FQBN="${1#*=}"
      shift
      ;;
    --arduino-fqbn)
      ARDUINO_FQBN="${2:-}"
      if [[ -z "${ARDUINO_FQBN}" ]]; then
        echo "Missing value for --arduino-fqbn" >&2
        exit 2
      fi
      shift 2
      ;;
    --arduino-reset-gpio=*)
      ARDUINO_RESET_GPIO="${1#*=}"
      shift
      ;;
    --arduino-reset-gpio)
      ARDUINO_RESET_GPIO="${2:-}"
      if [[ -z "${ARDUINO_RESET_GPIO}" ]]; then
        echo "Missing value for --arduino-reset-gpio" >&2
        exit 2
      fi
      shift 2
      ;;
    --arduino-baud=*)
      ARDUINO_BAUD="${1#*=}"
      shift
      ;;
    --arduino-baud)
      ARDUINO_BAUD="${2:-}"
      if [[ -z "${ARDUINO_BAUD}" ]]; then
        echo "Missing value for --arduino-baud" >&2
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
if [[ "${MODE}" == "spi" || "${MODE}" == "both" ]]; then
  echo "WARNING: SPI mode is dormant; forcing --mode hdmi."
  MODE="hdmi"
fi
if [[ "${MODE}" != "hdmi" ]]; then
  echo "Invalid --mode: ${MODE}. Expected hdmi." >&2
  exit 2
fi

BOOT_SPLASH="$(printf "%s" "${BOOT_SPLASH}" | tr '[:upper:]' '[:lower:]')"
if [[ "${BOOT_SPLASH}" == "spi" || "${BOOT_SPLASH}" == "both" ]]; then
  echo "WARNING: SPI boot splash is dormant; disabling boot splash."
  BOOT_SPLASH="none"
fi
if [[ "${BOOT_SPLASH}" != "auto" && "${BOOT_SPLASH}" != "none" && "${BOOT_SPLASH}" != "hdmi" ]]; then
  echo "Invalid --boot-splash: ${BOOT_SPLASH}. Expected auto, none, hdmi." >&2
  exit 2
fi
if [[ "${BOOT_SPLASH}" == "auto" ]]; then
  BOOT_SPLASH="${MODE}"
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo bash install.sh" >&2
  exit 1
fi

cleanup_space() {
  apt-get clean || true
  apt-get autoclean -y || true
  rm -rf /var/lib/apt/lists/* || true
  rm -rf /var/cache/apt/archives/partial/* || true
  rm -f /var/cache/apt/archives/*.deb || true
  rm -rf /tmp/pip-* || true
}

detect_arduino_port() {
  local candidate
  if [[ -e /dev/serial0 ]]; then
    printf "%s\n" "/dev/serial0"
    return 0
  fi
  for candidate in /dev/ttyACM* /dev/ttyUSB*; do
    if [[ -e "${candidate}" ]]; then
      printf "%s\n" "${candidate}"
      return 0
    fi
  done
  return 1
}

echo "[0/10] Pre-cleaning apt caches..."
cleanup_space
trap cleanup_space EXIT

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
POWER_SERVICE_FILE="/etc/systemd/system/gauge-power-control.service"
IR_PROTOCOL_SERVICE_FILE="/etc/systemd/system/gauge-ir-protocol.service"
LEGACY_SERVICE_FILE="/etc/systemd/system/gauge-cluster.service"
LIGHTDM_AUTOLOGIN_CONF="/etc/lightdm/lightdm.conf.d/99-gauge-autologin.conf"

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

remove_config_lines_matching() {
  local file="$1"
  local pattern="$2"
  local tmp
  tmp="$(mktemp)"
  awk -v pat="${pattern}" '$0 !~ pat' "${file}" > "${tmp}"
  cat "${tmp}" > "${file}"
  rm -f "${tmp}"
}

set_config_line_enabled() {
  local file="$1"
  local line="$2"
  # Some config directives include an extra "=" in the key portion
  # (for example "dtparam=i2c_arm=on" or "dtoverlay=gpio-ir,gpio_pin=26").
  # Match on everything before the final "=" so only the intended setting
  # is replaced.
  local key="${line%=*}"
  # Remove any existing active or commented assignment for this key,
  # then write exactly one enabled line.
  remove_config_lines_matching "${file}" "^[[:space:]]*#?[[:space:]]*${key}="
  append_line_if_missing "${file}" "${line}"
}

stop_runtime_services_for_arduino() {
  # Stop dashboard/runtime services before flashing so app processes
  # release serial0.
  local unit
  for unit in \
    gauge-cluster.service \
    gauge-cluster-spi.service \
    gauge-cluster-hdmi.service \
    gauge-hide-cursor.service \
    gauge-power-control.service; do
    systemctl stop "${unit}" >/dev/null 2>&1 || true
  done
  # Also stop any manually launched dashboard process not tracked by systemd.
  pkill -f "${PROJECT_DIR}/src/main.py" >/dev/null 2>&1 || true
  pkill -f "${PROJECT_DIR}/scripts/run_hw.sh" >/dev/null 2>&1 || true
  pkill -f "${PROJECT_DIR}/scripts/run_sim.sh" >/dev/null 2>&1 || true
}

wait_for_serial_port_free() {
  local port="$1"
  local timeout_s="${2:-20}"
  local waited=0
  if [[ -z "${port}" || ! -e "${port}" ]]; then
    return 0
  fi
  if ! command -v fuser >/dev/null 2>&1; then
    return 0
  fi
  while fuser "${port}" >/dev/null 2>&1; do
    if [[ "${waited}" -eq 0 ]]; then
      echo "Waiting for ${port} to be released..."
    fi
    sleep 1
    waited=$((waited + 1))
    if [[ "${waited}" -ge "${timeout_s}" ]]; then
      echo "WARNING: ${port} is still busy after ${timeout_s}s; flashing may fail." >&2
      fuser -v "${port}" || true
      break
    fi
  done
}

remove_cmdline_token_prefix() {
  local file="$1"
  local prefix="$2"
  local current updated
  current="$(<"${file}")"
  updated="$(printf "%s\n" "${current}" | sed -E "s/(^| )${prefix}[^ ]*//g; s/  +/ /g; s/^ //; s/ $//")"
  printf "%s\n" "${updated}" > "${file}"
}

disable_serial_login_console() {
  # Keep UART hardware enabled for Arduino comms, but disable serial login/console.
  remove_cmdline_token_prefix "${CMDLINE_TXT}" "console=serial0"
  remove_cmdline_token_prefix "${CMDLINE_TXT}" "console=ttyAMA0"
  remove_cmdline_token_prefix "${CMDLINE_TXT}" "console=ttyS0"

  local unit
  for unit in \
    serial-getty@serial0.service \
    serial-getty@ttyAMA0.service \
    serial-getty@ttyS0.service; do
    systemctl stop "${unit}" >/dev/null 2>&1 || true
    systemctl disable "${unit}" >/dev/null 2>&1 || true
    systemctl mask "${unit}" >/dev/null 2>&1 || true
  done
}

detect_hdmi_boot_connector() {
  local status_file
  for status_file in /sys/class/drm/card*-HDMI-A-*/status; do
    [[ -e "${status_file}" ]] || continue
    if [[ "$(<"${status_file}")" == "connected" ]]; then
      local node
      node="$(basename "$(dirname "${status_file}")")"
      # Convert "card1-HDMI-A-2" -> "HDMI-A-2" for kernel video= token.
      echo "${node#*-}"
      return 0
    fi
  done
  # Most Raspberry Pi 4/5 kiosk wiring uses HDMI-2.
  echo "HDMI-A-2"
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

bootstrap_bluetooth_adapter() {
  if ! command -v bluetoothctl >/dev/null 2>&1; then
    echo "  WARNING: bluetoothctl not found; skipping adapter bootstrap."
    return 0
  fi

  systemctl unmask bluetooth.service >/dev/null 2>&1 || true
  systemctl enable bluetooth.service >/dev/null 2>&1 || true
  systemctl restart bluetooth.service >/dev/null 2>&1 || true

  if command -v rfkill >/dev/null 2>&1; then
    rfkill unblock bluetooth >/dev/null 2>&1 || true
  fi

  # Some images boot with hci down; this is best-effort and harmless if absent.
  if command -v hciconfig >/dev/null 2>&1; then
    hciconfig hci0 up >/dev/null 2>&1 || true
  fi

  if ! bluetoothctl show | rg -q "Controller "; then
    echo "  WARNING: No Bluetooth controller detected by bluez."
    return 0
  fi

  bluetoothctl <<'EOF' >/dev/null 2>&1 || true
power on
agent NoInputNoOutput
default-agent
pairable on
pairable-timeout 0
discoverable-timeout 0
discoverable on
EOF
}

echo "[1/10] Installing system packages..."
if [[ "${SKIP_PACKAGES}" -eq 1 ]]; then
  echo "Skipping package install (--skip-packages)"
else
  apt-get update
  apt-get install "${APT_INSTALL_ARGS[@]}" \
    python3-pip python3-venv \
    python3-pygame python3-pil python3-spidev python3-rpi.gpio python3-evdev \
    ir-keytable i2c-tools alsa-utils raspi-config \
    gcc-avr binutils-avr avr-libc \
    rsync x11-utils x11-xserver-utils xinit xserver-xorg lightdm openbox xterm \
    bluez pulseaudio pulseaudio-module-bluetooth \
    navit gpsd gpsd-clients \
    plymouth plymouth-themes unclutter \
    curl ca-certificates avrdude
  systemctl enable lightdm.service >/dev/null 2>&1 || true
  bootstrap_bluetooth_adapter
fi

echo "[2/10] Configuring boot settings..."
if [[ "${SKIP_ARDUINO}" -eq 0 ]]; then
  set_config_line_enabled "${CONFIG_TXT}" "enable_uart=1"
fi
# FM radio (Si4737) rides I2C bus 1; ensure it's enabled for all install modes.
set_config_line_enabled "${CONFIG_TXT}" "dtparam=i2c_arm=on"
# Ensure KMS graphics stack is enabled so LightDM can create X display :0.
remove_config_lines_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=vc4-fkms-v3d.*$"
set_config_line_enabled "${CONFIG_TXT}" "dtoverlay=vc4-kms-v3d"
if command -v raspi-config >/dev/null 2>&1; then
  # Keep firmware-managed I2C state in sync with config.txt.
  raspi-config nonint do_i2c 0 >/dev/null 2>&1 || true
fi
# Best-effort immediate availability without reboot; boot-time enable still
# requires reboot for persistence.
modprobe i2c-dev >/dev/null 2>&1 || true
modprobe i2c-bcm2835 >/dev/null 2>&1 || modprobe i2c_bcm2835 >/dev/null 2>&1 || true
if [[ ! -e /dev/i2c-1 ]]; then
  echo "WARNING: /dev/i2c-1 not present yet. Reboot after install to apply i2c_arm changes." >&2
fi
if [[ ! -e /dev/dri/card0 ]]; then
  echo "WARNING: /dev/dri/card0 not present yet. Reboot after install to apply KMS graphics changes." >&2
fi
# Use Raspberry Pi onboard analog audio jack by default.
# Remove external I2S DAC overlays if present.
remove_config_lines_matching "${CONFIG_TXT}" "^[[:space:]]*dtparam=i2s=on[[:space:]]*$"
remove_config_lines_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=hifiberry-dacplus.*$"
remove_config_lines_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=rpi-dac.*$"
remove_config_lines_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=hifiberry-dac[[:space:]]*$"
set_config_line_enabled "${CONFIG_TXT}" "dtparam=audio=on"
disable_serial_login_console
# Keep SPI0 enabled for ATtiny ISP programming, even in HDMI deployments.
set_config_line_enabled "${CONFIG_TXT}" "dtparam=spi=on"
# SPI display mode is intentionally dormant in HDMI-only deployments.
# Avoid reserving SPI1 display pins/buffers in config/cmdline.
remove_config_lines_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=spi1-3cs[[:space:]]*$"
remove_cmdline_token_prefix "${CMDLINE_TXT}" "spidev.bufsiz="
# Force 3.5mm analog jack output path (disable HDMI PCM endpoint in snd_bcm2835).
remove_cmdline_token_prefix "${CMDLINE_TXT}" "snd_bcm2835.enable_headphones="
remove_cmdline_token_prefix "${CMDLINE_TXT}" "snd_bcm2835.enable_hdmi="
ensure_cmdline_token "${CMDLINE_TXT}" "snd_bcm2835.enable_headphones=1"
ensure_cmdline_token "${CMDLINE_TXT}" "snd_bcm2835.enable_hdmi=0"
# Enable GPIO IR receiver for dashboard remote input.
remove_config_lines_matching "${CONFIG_TXT}" "^dtoverlay=gpio-ir,gpio_pin="
append_line_if_missing "${CONFIG_TXT}" "dtoverlay=gpio-ir,gpio_pin=${IR_GPIO_PIN}"

if [[ "${MODE}" == "hdmi" || "${BOOT_SPLASH}" == "hdmi" ]]; then
  HDMI_BOOT_CONNECTOR="$(detect_hdmi_boot_connector)"
  append_line_if_missing "${CONFIG_TXT}" "hdmi_force_hotplug=1"
  append_line_if_missing "${CONFIG_TXT}" "hdmi_group=2"
  append_line_if_missing "${CONFIG_TXT}" "hdmi_mode=87"
  append_line_if_missing "${CONFIG_TXT}" "hdmi_cvt=800 480 60 6 0 0 0"
  append_line_if_missing "${CONFIG_TXT}" "disable_overscan=1"
  # Force early-kernel HDMI mode on the physically connected panel so
  # Plymouth does not render to the wrong output.
  remove_cmdline_token_prefix "${CMDLINE_TXT}" "video="
  remove_cmdline_token_prefix "${CMDLINE_TXT}" "video=HDMI-A-1:"
  remove_cmdline_token_prefix "${CMDLINE_TXT}" "video=HDMI-A-2:"
  ensure_cmdline_token "${CMDLINE_TXT}" "video=${HDMI_BOOT_CONNECTOR}:800x480M@60D"
fi

if [[ "${MODE}" == "hdmi" ]]; then
  install -d /etc/lightdm/lightdm.conf.d
  cat > "${LIGHTDM_AUTOLOGIN_CONF}" <<LIGHTDMCONF
[Seat:*]
autologin-user=${TARGET_USER}
autologin-user-timeout=0
user-session=lightdm-xsession
autologin-session=lightdm-xsession
LIGHTDMCONF

  cat > "${TARGET_HOME}/.xsession" <<'XSESSION'
#!/usr/bin/env sh
set -eu

if command -v openbox-session >/dev/null 2>&1; then
  exec openbox-session
fi
if command -v openbox >/dev/null 2>&1; then
  exec openbox
fi
if command -v x-session-manager >/dev/null 2>&1; then
  exec x-session-manager
fi
if command -v x-window-manager >/dev/null 2>&1; then
  exec x-window-manager
fi
exec x-terminal-emulator
XSESSION
  chown "${TARGET_USER}:${TARGET_USER}" "${TARGET_HOME}/.xsession"
  chmod 755 "${TARGET_HOME}/.xsession"
fi

echo "[3/10] Syncing project..."
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

echo "[4/10] Building Python environment..."
if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
  sudo -u "${TARGET_USER}" python3 -m venv --system-site-packages "${VENV_DIR}"
fi
if [[ -f "${PROJECT_DIR}/requirements.txt" ]]; then
  sudo -u "${TARGET_USER}" "${VENV_DIR}/bin/python" -m pip install -r "${PROJECT_DIR}/requirements.txt"
fi
rm -rf "${TARGET_HOME}/.cache/pip" || true

echo "[5/10] Installing Arduino CLI and flashing firmware..."
if [[ "${SKIP_ARDUINO}" -eq 1 ]]; then
  echo "Skipping Arduino update (--skip-arduino)"
else
  ARDUINO_FW_STATUS="updated"
  ARDUINO_FW_FAILED=0
  stop_runtime_services_for_arduino
  if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "Installing arduino-cli (official installer)..."
    if ! curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=/usr/local/bin sh; then
      echo "WARNING: Failed to install arduino-cli." >&2
      ARDUINO_FW_FAILED=1
    fi
  else
    echo "arduino-cli already installed."
  fi

  if [[ "${ARDUINO_FW_FAILED}" -eq 0 ]]; then
    echo "Updating Arduino board index..."
    if ! arduino-cli core update-index; then
      echo "WARNING: Failed to update Arduino board index." >&2
      ARDUINO_FW_FAILED=1
    fi
  fi
  if [[ "${ARDUINO_FW_FAILED}" -eq 0 ]]; then
    if ! arduino-cli core list | awk '{print $1}' | rg "^arduino:avr$" >/dev/null 2>&1; then
      echo "Installing Arduino core: arduino:avr"
      if ! arduino-cli core install arduino:avr; then
        echo "WARNING: Failed to install Arduino core arduino:avr." >&2
        ARDUINO_FW_FAILED=1
      fi
    else
      echo "Arduino core already installed: arduino:avr"
    fi
  fi

  UPLOAD_PORT="${ARDUINO_PORT}"
  if [[ "${ARDUINO_FW_FAILED}" -eq 0 && "${UPLOAD_PORT}" == "auto" ]]; then
    if ! UPLOAD_PORT="$(detect_arduino_port)"; then
      echo "WARNING: No Arduino serial port found at /dev/serial0, /dev/ttyACM*, or /dev/ttyUSB*." >&2
      echo "         Reconnect board and rerun, or specify --arduino-port /dev/serial0 (UART) or /dev/ttyACM0 (USB)." >&2
      ARDUINO_FW_FAILED=1
    else
      echo "Auto-detected Arduino port: ${UPLOAD_PORT}"
    fi
  fi
  ARDUINO_PORT="${UPLOAD_PORT}"
  if [[ "${ARDUINO_FW_FAILED}" -eq 0 ]]; then
    wait_for_serial_port_free "${ARDUINO_PORT}" 20
  fi
  if [[ "${ARDUINO_FW_FAILED}" -eq 0 && ( "${UPLOAD_PORT}" == "/dev/serial0" || "${UPLOAD_PORT}" == "/dev/ttyAMA0" || "${UPLOAD_PORT}" == "/dev/ttyS0" ) ]]; then
    SKETCH_DIR="${PROJECT_DIR}/arduino/sketches/sensor_pack"
    BUILD_DIR="${PROJECT_DIR}/arduino/build/sensor_pack"
    HEX_FILE="${BUILD_DIR}/sensor_pack.ino.hex"
    MCU="atmega328p"

    mkdir -p "${BUILD_DIR}"
    echo "Compiling sensor_pack for ${ARDUINO_FQBN}..."
    if ! arduino-cli compile \
      --fqbn "${ARDUINO_FQBN}" \
      --build-path "${BUILD_DIR}" \
      "${SKETCH_DIR}"; then
      echo "WARNING: Arduino compile failed." >&2
      ARDUINO_FW_FAILED=1
    fi

    if [[ "${ARDUINO_FW_FAILED}" -eq 0 && ! -f "${HEX_FILE}" ]]; then
      shopt -s nullglob
      HEX_CANDIDATES=("${BUILD_DIR}"/*.ino.hex)
      shopt -u nullglob
      if [[ "${#HEX_CANDIDATES[@]}" -lt 1 ]]; then
        echo "WARNING: Failed to locate compiled hex in ${BUILD_DIR}" >&2
        ARDUINO_FW_FAILED=1
      else
        HEX_FILE="${HEX_CANDIDATES[0]}"
      fi
    fi

    if [[ "${ARDUINO_FW_FAILED}" -eq 0 ]]; then
      echo "Pulsing GPIO ${ARDUINO_RESET_GPIO} to reset ATmega bootloader..."
    fi
    if [[ "${ARDUINO_FW_FAILED}" -eq 0 && -f "${CMDLINE_TXT}" ]]; then
      CURRENT_CMDLINE="$(<"${CMDLINE_TXT}")"
      if [[ "${CURRENT_CMDLINE}" == *"console=serial0,"* || "${CURRENT_CMDLINE}" == *"console=ttyAMA0,"* ]]; then
        echo "WARNING: Serial console appears enabled in ${CMDLINE_TXT}."
        echo "         Disable login shell over serial in raspi-config for reliable UART flashing."
      fi
    fi
    if [[ "${ARDUINO_FW_FAILED}" -eq 0 ]]; then
      python3 - "${ARDUINO_RESET_GPIO}" <<'PY'
import sys
import time
import RPi.GPIO as GPIO

pin = int(sys.argv[1])
GPIO.setwarnings(False)
GPIO.setmode(GPIO.BCM)
GPIO.setup(pin, GPIO.OUT, initial=GPIO.HIGH)
GPIO.output(pin, GPIO.LOW)
time.sleep(0.1)
GPIO.output(pin, GPIO.HIGH)
time.sleep(0.1)
GPIO.cleanup(pin)
PY
    fi

    if [[ "${ARDUINO_FW_FAILED}" -eq 0 ]]; then
      echo "Flashing ${HEX_FILE} to ${UPLOAD_PORT} (${MCU} @ ${ARDUINO_BAUD})..."
      if ! avrdude -v \
        -p "${MCU}" \
        -c arduino \
        -P "${UPLOAD_PORT}" \
        -b "${ARDUINO_BAUD}" \
        -D \
        -U "flash:w:${HEX_FILE}:i"; then
        echo "WARNING: Arduino flash failed on ${UPLOAD_PORT}." >&2
        ARDUINO_FW_FAILED=1
      fi
    fi
  elif [[ "${ARDUINO_FW_FAILED}" -eq 0 ]]; then
    if ! bash "${PROJECT_DIR}/arduino/scripts/upload_sensor_pack.sh" "${UPLOAD_PORT}" "${ARDUINO_FQBN}"; then
      echo "WARNING: Arduino upload script failed." >&2
      ARDUINO_FW_FAILED=1
    fi
  fi

  if [[ "${ARDUINO_FW_FAILED}" -ne 0 ]]; then
    ARDUINO_FW_STATUS="failed"
    echo "WARNING: Arduino firmware update failed; continuing install." >&2
  fi
fi

echo "[6/10] Writing services..."
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
ExecStart=/bin/bash -lc 'set -euo pipefail; LOG=/boot/elc-gauge-spi.log; [ -d /boot/firmware ] && LOG=/boot/firmware/elc-gauge-spi.log; touch "\$LOG"; echo "[elc] gauge-cluster-spi start \$(date -Is)" >> "\$LOG"; exec ${VENV_DIR}/bin/python ${PROJECT_DIR}/src/main.py --config ${PROJECT_DIR}/config/dashboard.spi.yaml --render-mode spi --headless >>"\$LOG" 2>&1'
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
Environment=XDG_RUNTIME_DIR=/run/user/$(id -u "${TARGET_USER}")
Environment=DISPLAY_MODE=hardware
Environment=SDL_VIDEODRIVER=x11
Environment=SDL_AUDIODRIVER=alsa
Environment=PYTHONPATH=${PROJECT_DIR}/src
ExecStart=/bin/bash -lc 'set -euo pipefail; LOG=/boot/elc-gauge-hdmi.log; [ -d /boot/firmware ] && LOG=/boot/firmware/elc-gauge-hdmi.log; touch "\$LOG"; echo "[elc] gauge-cluster-hdmi start \$(date -Is)" >> "\$LOG"; export DISPLAY=:0; export XAUTHORITY=${TARGET_HOME}/.Xauthority; export XDG_RUNTIME_DIR=/run/user/$(id -u "${TARGET_USER}"); export SDL_VIDEODRIVER=x11; export SDL_AUDIODRIVER=alsa; until [ -S /tmp/.X11-unix/X0 ] && [ -f "${TARGET_HOME}/.Xauthority" ] && xset q >/dev/null 2>&1; do sleep 1; done; cd ${PROJECT_DIR}; exec ${VENV_DIR}/bin/python ${PROJECT_DIR}/src/main.py --config ${PROJECT_DIR}/config/dashboard.hdmi5.yaml >>"\$LOG" 2>&1'
Restart=always
RestartSec=5

[Install]
WantedBy=graphical.target
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
ExecStart=/bin/bash -lc 'export DISPLAY=:0; export XAUTHORITY=${TARGET_HOME}/.Xauthority; until [ -S /tmp/.X11-unix/X0 ] && [ -f "${TARGET_HOME}/.Xauthority" ] && xset q >/dev/null 2>&1; do sleep 1; done; exec /usr/bin/unclutter -display :0 -idle 0 -root -grab'
Restart=always
RestartSec=2

[Install]
WantedBy=graphical.target
SERVICECURSOR

cat > "${POWER_SERVICE_FILE}" <<SERVICEPOWER
[Unit]
Description=Gauge Power Control Listener
After=multi-user.target
Wants=multi-user.target

[Service]
Type=simple
User=root
WorkingDirectory=${PROJECT_DIR}
ExecStart=/bin/bash -lc 'set -euo pipefail; LOG=/boot/elc-power-control.log; [ -d /boot/firmware ] && LOG=/boot/firmware/elc-power-control.log; touch "\$LOG"; echo "[elc] gauge-power-control start \$(date -Is)" >> "\$LOG"; exec ${VENV_DIR}/bin/python ${PROJECT_DIR}/scripts/power_control_listener.py --signal-pin 16 --confirm-pin 12 --debounce-ms 80 --poll-interval-ms 20 >>"\$LOG" 2>&1'
Restart=always
RestartSec=2
KillSignal=SIGTERM
TimeoutStopSec=5

[Install]
WantedBy=multi-user.target
SERVICEPOWER

cat > "${IR_PROTOCOL_SERVICE_FILE}" <<SERVICEIR
[Unit]
Description=Apply IR decoder protocols for gpio_ir_recv
After=multi-user.target
Before=gauge-cluster-hdmi.service
Wants=multi-user.target

[Service]
Type=oneshot
User=root
WorkingDirectory=${PROJECT_DIR}
ExecStart=/bin/bash -lc 'set -euo pipefail; LOG=/boot/elc-ir-protocol.log; [ -d /boot/firmware ] && LOG=/boot/firmware/elc-ir-protocol.log; touch "\$LOG"; echo "[elc] gauge-ir-protocol start \$(date -Is)" >> "\$LOG"; exec ${VENV_DIR}/bin/python ${PROJECT_DIR}/scripts/apply_ir_protocols.py --config ${PROJECT_DIR}/config/dashboard.hdmi5.yaml >>"\$LOG" 2>&1'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
SERVICEIR

rm -f "${LEGACY_SERVICE_FILE}" || true

echo "[6.5/10] Installing MCP diagnostics helper..."
install -m 755 "${PROJECT_DIR}/scripts/gauge-mcp-monitor" /usr/local/bin/gauge-mcp-monitor
systemctl daemon-reload

echo "[7/10] Applying mode selection..."
systemctl disable gauge-cluster.service >/dev/null 2>&1 || true
systemctl stop gauge-cluster.service >/dev/null 2>&1 || true
# Clear stale target links from previous unit install sections.
systemctl disable gauge-cluster-spi.service >/dev/null 2>&1 || true
systemctl disable gauge-cluster-hdmi.service >/dev/null 2>&1 || true
systemctl disable gauge-hide-cursor.service >/dev/null 2>&1 || true
systemctl disable gauge-power-control.service >/dev/null 2>&1 || true
systemctl disable gauge-ir-protocol.service >/dev/null 2>&1 || true

systemctl set-default graphical.target || true
systemctl mask gauge-cluster-spi.service >/dev/null 2>&1 || true
systemctl disable gauge-cluster-spi.service >/dev/null 2>&1 || true
systemctl stop gauge-cluster-spi.service >/dev/null 2>&1 || true
systemctl enable gauge-cluster-hdmi.service
systemctl enable gauge-hide-cursor.service
systemctl enable gauge-power-control.service
systemctl enable gauge-ir-protocol.service
systemctl restart gauge-ir-protocol.service || true
systemctl restart gauge-cluster-hdmi.service || true
systemctl restart gauge-hide-cursor.service || true
systemctl restart gauge-power-control.service || true

echo "[8/10] Applying boot splash selection..."
systemctl mask gauge-boot-splash.service >/dev/null 2>&1 || true
systemctl disable gauge-boot-splash.service >/dev/null 2>&1 || true
systemctl stop gauge-boot-splash.service >/dev/null 2>&1 || true

if [[ "${MODE}" == "hdmi" || "${BOOT_SPLASH}" == "hdmi" ]]; then
  bash "${PROJECT_DIR}/scripts/setup_boot_image.sh"
fi

echo "[9/10] Verifying Navit + Bluetooth audio prerequisites..."
if command -v navit >/dev/null 2>&1; then
  echo "  Navit executable found."
else
  echo "  WARNING: navit command not found. Install package 'navit' or set integrations.navit.executable."
fi

if command -v bluetoothctl >/dev/null 2>&1; then
  echo "  bluetoothctl found."
  echo "  Bootstrapping adapter (power/pairable/discoverable)..."
  bootstrap_bluetooth_adapter
  BT_SHOW_OUTPUT="$(bluetoothctl show 2>/dev/null || true)"
  if [[ -n "${BT_SHOW_OUTPUT}" ]]; then
    echo "${BT_SHOW_OUTPUT}" | rg "Controller|Powered:|Discoverable:|Pairable:" || true
  fi
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
usermod -aG i2c "${TARGET_USER}" >/dev/null 2>&1 || true
usermod -aG gpio "${TARGET_USER}" >/dev/null 2>&1 || true
usermod -aG input "${TARGET_USER}" >/dev/null 2>&1 || true

echo "[10/10] Final apt cache cleanup..."
cleanup_space

echo
echo "Fresh install complete."
echo "Mode: ${MODE}"
echo "Boot splash: ${BOOT_SPLASH}"
echo "Project: ${PROJECT_DIR}"
echo "Arduino firmware: ${ARDUINO_FW_STATUS}"
if [[ "${SKIP_ARDUINO}" -eq 0 ]]; then
  echo "Arduino FQBN: ${ARDUINO_FQBN}"
  echo "Arduino port: ${ARDUINO_PORT}"
  if [[ "${ARDUINO_PORT}" == "/dev/serial0" || "${ARDUINO_PORT}" == "/dev/ttyAMA0" || "${ARDUINO_PORT}" == "/dev/ttyS0" ]]; then
    echo "Arduino reset GPIO: ${ARDUINO_RESET_GPIO}"
    echo "Arduino baud: ${ARDUINO_BAUD}"
  fi
fi
echo
echo "Navit/Bluetooth notes:"
echo "  - Enable integrations.navit.enabled and integrations.bluetooth_audio.enabled in config"
echo "  - Bluetooth pairing/connection uses bluetoothctl and PulseAudio sinks via pactl"
echo "  - Reboot recommended so audio/bluetooth group changes fully apply"
echo
echo "Quick checks:"
echo "  systemctl status gauge-cluster-hdmi.service -l --no-pager"
echo "  systemctl status gauge-power-control.service -l --no-pager"
echo "  systemctl is-enabled gauge-cluster-spi.service"
echo "  bluetoothctl show"
echo "  navit --version"
echo "  ir-keytable"
echo "  ir-keytable -t"
echo "  aplay -l"
echo "  ls -l /dev/spidev*"
echo "  ls -l /dev/i2c*"
echo "  i2cdetect -y 1"
echo "  systemctl stop gauge-cluster-hdmi.service && gauge-mcp-monitor --samples 250"
echo
echo "Reboot recommended:"
echo "  sudo reboot"
