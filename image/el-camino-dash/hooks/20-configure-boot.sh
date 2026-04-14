#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_rootfs

# Force deterministic hostname for deployed dashboard images.
echo "digidash" > "$(in_rootfs /etc/hostname)"
if [[ -f "$(in_rootfs /etc/hosts)" ]]; then
  if grep -Eq '^[[:space:]]*127\.0\.1\.1[[:space:]]+' "$(in_rootfs /etc/hosts)"; then
    sed -i -E 's/^[[:space:]]*127\.0\.1\.1[[:space:]]+.*/127.0.1.1\tdigidash/' "$(in_rootfs /etc/hosts)"
  else
    printf "127.0.1.1\tdigidash\n" >> "$(in_rootfs /etc/hosts)"
  fi
fi

BOOT_DIR=""
if [[ -f "$(in_rootfs /boot/firmware/config.txt)" ]]; then
  BOOT_DIR="$(in_rootfs /boot/firmware)"
elif [[ -f "$(in_rootfs /boot/config.txt)" ]]; then
  BOOT_DIR="$(in_rootfs /boot)"
else
  echo "Skipping boot config hook: config.txt not present in rootfs yet." >&2
  exit 0
fi

CONFIG_TXT="${BOOT_DIR}/config.txt"
CMDLINE_TXT="${BOOT_DIR}/cmdline.txt"

append_line_if_missing() {
  local file="$1"
  local line="$2"
  if ! grep -Fqx "${line}" "${file}"; then
    printf "\n%s\n" "${line}" >> "${file}"
  fi
}

remove_matching() {
  local file="$1"
  local pattern="$2"
  local tmp
  tmp="$(mktemp)"
  awk -v pat="${pattern}" '$0 !~ pat' "${file}" > "${tmp}"
  cp "${tmp}" "${file}"
  rm -f "${tmp}"
}

remove_cmdline_token_prefix() {
  local file="$1"
  local prefix="$2"
  local current updated
  current="$(<"${file}")"
  updated="$(printf "%s\n" "${current}" | sed -E "s/(^| )${prefix}[^ ]*//g; s/  +/ /g; s/^ //; s/ $//")"
  printf "%s\n" "${updated}" > "${file}"
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

mask_unit_in_rootfs() {
  local unit="$1"
  local unit_path
  unit_path="$(in_rootfs "/etc/systemd/system/${unit}")"
  mkdir -p "$(dirname "${unit_path}")"
  ln -sf /dev/null "${unit_path}"
}

append_line_if_missing "${CONFIG_TXT}" "enable_uart=1"
# Vehicle UART: sensor pack (UART1) + encoder / nano bridge (UART4). GPIO pins from overlay help:
#   dtoverlay -h uart1   /   dtoverlay -h uart4
append_line_if_missing "${CONFIG_TXT}" "dtoverlay=uart1"
append_line_if_missing "${CONFIG_TXT}" "dtoverlay=uart4"
# I2C: FM, audio router, etc. — not used for the 5V ATmega328P sensor pack (UART1 only in app config).
append_line_if_missing "${CONFIG_TXT}" "dtparam=i2c_arm=on"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=disable-bt[[:space:]]*$"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=miniuart-bt[[:space:]]*$"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=disable-wifi[[:space:]]*$"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=vc4-fkms-v3d.*$"
append_line_if_missing "${CONFIG_TXT}" "dtoverlay=vc4-kms-v3d"
append_line_if_missing "${CONFIG_TXT}" "dtparam=audio=on"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*dtparam=spi=on[[:space:]]*$"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*dtparam=spi=off[[:space:]]*$"
append_line_if_missing "${CONFIG_TXT}" "dtparam=spi=off"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=spi1-3cs[[:space:]]*$"
remove_matching "${CONFIG_TXT}" "^dtoverlay=gpio-ir,gpio_pin="
append_line_if_missing "${CONFIG_TXT}" "dtoverlay=gpio-ir,gpio_pin=26"
# HDMI: replace globals with a single ELC block (dual ports: splitter + 4in).
remove_matching "${CONFIG_TXT}" "^[[:space:]]*hdmi_force_hotplug=.*$"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*hdmi_group=.*$"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*hdmi_mode=.*$"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*hdmi_cvt=.*$"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*disable_overscan=.*$"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*display_hdmi_rotate=.*$"
if [[ -f "${CONFIG_TXT}" ]] && grep -q "# ELC-HDMI-BEGIN" "${CONFIG_TXT}"; then
  sed -i '/# ELC-HDMI-BEGIN/,/# ELC-HDMI-END/d' "${CONFIG_TXT}"
fi
cat >> "${CONFIG_TXT}" <<'HDMI_EOF'
# ELC-HDMI-BEGIN — HDMI0 = wide splitter (2880×1080); HDMI1 = 4in round (720×720).
# Physical: Pi HDMI0 is usually the port nearest the USB-C (Pi 4/5); HDMI1 is the second port.
# If the 4″ is on the “wrong” socket, swap the two [HDMI:n] blocks below (same timings, swapped sections).
hdmi_force_hotplug=1
disable_overscan=1
# Avoid 4K60 negotiation quirks on dual-HDMI setups (Bookworm / vc4-kms-v3d).
hdmi_enable_4kp60=0

[HDMI:0]
hdmi_group=2
hdmi_mode=87
hdmi_cvt=2880 1080 60 6 0 0 0

[HDMI:1]
hdmi_group=2
hdmi_mode=87
hdmi_cvt=720 720 60 6 0 0 0
# Upside-down bezel: use xrandr (ELC_X11_ROTATE) in gauge-cluster-hdmi-x11-extended, not display_hdmi_rotate here.

# ELC-HDMI-END
HDMI_EOF

remove_cmdline_token_prefix "${CMDLINE_TXT}" "console=serial0"
remove_cmdline_token_prefix "${CMDLINE_TXT}" "console=ttyAMA0"
remove_cmdline_token_prefix "${CMDLINE_TXT}" "console=ttyS0"
remove_cmdline_token_prefix "${CMDLINE_TXT}" "spidev.bufsiz="
remove_cmdline_token_prefix "${CMDLINE_TXT}" "snd_bcm2835.enable_headphones="
remove_cmdline_token_prefix "${CMDLINE_TXT}" "snd_bcm2835.enable_hdmi="
remove_cmdline_token_prefix "${CMDLINE_TXT}" "video="

ensure_cmdline_token "${CMDLINE_TXT}" "snd_bcm2835.enable_headphones=1"
ensure_cmdline_token "${CMDLINE_TXT}" "snd_bcm2835.enable_hdmi=0"
# Do not set video=HDMI-A-* here — it forces one connector and often blanks the other.
# Modes come from config.txt [HDMI:0]/[HDMI:1]; DRM/KMS picks both up.

# Keep UART enabled but guarantee Linux login consoles do not bind serial ports.
mask_unit_in_rootfs "serial-getty@serial0.service"
mask_unit_in_rootfs "serial-getty@ttyAMA0.service"
mask_unit_in_rootfs "serial-getty@ttyS0.service"
mask_unit_in_rootfs "serial-getty@ttyAMA1.service"
mask_unit_in_rootfs "serial-getty@ttyAMA4.service"
# Ensure BT UART attach is available when bluetooth service is started late.
rm -f "$(in_rootfs /etc/systemd/system/hciuart.service)"

# Ensure userspace I2C device nodes are available for MCP23017 input.
mkdir -p "$(in_rootfs /etc/modules-load.d)"
cat > "$(in_rootfs /etc/modules-load.d/elc-i2c.conf)" <<'EOF'
i2c-dev
EOF

# Pin dashboard image timezone from config (fallback Eastern).
TIMEZONE="${IGconf_locale_timezone:-America/New_York}"
if [[ -f "$(in_rootfs /usr/share/zoneinfo/${TIMEZONE})" ]]; then
  ln -sf "../usr/share/zoneinfo/${TIMEZONE}" "$(in_rootfs /etc/localtime)"
  printf "%s\n" "${TIMEZONE}" > "$(in_rootfs /etc/timezone)"
fi
