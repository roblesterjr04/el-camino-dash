#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_rootfs

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

append_line_if_missing "${CONFIG_TXT}" "enable_uart=1"
append_line_if_missing "${CONFIG_TXT}" "dtparam=i2c_arm=on"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=vc4-fkms-v3d.*$"
append_line_if_missing "${CONFIG_TXT}" "dtoverlay=vc4-kms-v3d"
append_line_if_missing "${CONFIG_TXT}" "dtparam=audio=on"
append_line_if_missing "${CONFIG_TXT}" "dtparam=spi=on"
remove_matching "${CONFIG_TXT}" "^[[:space:]]*dtoverlay=spi1-3cs[[:space:]]*$"
remove_matching "${CONFIG_TXT}" "^dtoverlay=gpio-ir,gpio_pin="
append_line_if_missing "${CONFIG_TXT}" "dtoverlay=gpio-ir,gpio_pin=26"
append_line_if_missing "${CONFIG_TXT}" "hdmi_force_hotplug=1"
append_line_if_missing "${CONFIG_TXT}" "hdmi_group=2"
append_line_if_missing "${CONFIG_TXT}" "hdmi_mode=87"
append_line_if_missing "${CONFIG_TXT}" "hdmi_cvt=800 480 60 6 0 0 0"
append_line_if_missing "${CONFIG_TXT}" "disable_overscan=1"

remove_cmdline_token_prefix "${CMDLINE_TXT}" "console=serial0"
remove_cmdline_token_prefix "${CMDLINE_TXT}" "console=ttyAMA0"
remove_cmdline_token_prefix "${CMDLINE_TXT}" "console=ttyS0"
remove_cmdline_token_prefix "${CMDLINE_TXT}" "spidev.bufsiz="
remove_cmdline_token_prefix "${CMDLINE_TXT}" "snd_bcm2835.enable_headphones="
remove_cmdline_token_prefix "${CMDLINE_TXT}" "snd_bcm2835.enable_hdmi="
remove_cmdline_token_prefix "${CMDLINE_TXT}" "video="

ensure_cmdline_token "${CMDLINE_TXT}" "snd_bcm2835.enable_headphones=1"
ensure_cmdline_token "${CMDLINE_TXT}" "snd_bcm2835.enable_hdmi=0"
ensure_cmdline_token "${CMDLINE_TXT}" "video=HDMI-A-2:800x480M@60D"
