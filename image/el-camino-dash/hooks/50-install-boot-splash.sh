#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_rootfs

SOURCE_IMAGE="${SOURCE_DIR}/assets/bootscreen/boot.png"
if [[ ! -f "${SOURCE_IMAGE}" ]]; then
  echo "Skipping boot splash hook: source image missing (${SOURCE_IMAGE})." >&2
  exit 0
fi

BOOT_DIR=""
if [[ -f "$(in_rootfs /boot/firmware/config.txt)" ]]; then
  BOOT_DIR="$(in_rootfs /boot/firmware)"
elif [[ -f "$(in_rootfs /boot/config.txt)" ]]; then
  BOOT_DIR="$(in_rootfs /boot)"
else
  echo "Skipping boot splash hook: rootfs boot config not found." >&2
  exit 0
fi

CONFIG_TXT="${BOOT_DIR}/config.txt"
CMDLINE_TXT="${BOOT_DIR}/cmdline.txt"
THEME_DIR="$(in_rootfs /usr/share/plymouth/themes/el-camino)"
PLYMOUTH_DEF="${THEME_DIR}/el-camino.plymouth"
PLYMOUTH_SCRIPT="${THEME_DIR}/el-camino.script"
THEME_IMAGE="${THEME_DIR}/boot_hdmi.png"
DEFAULT_THEME_LINK="$(in_rootfs /usr/share/plymouth/themes/default.plymouth)"
PLYMOUTH_CONF="$(in_rootfs /etc/plymouth/plymouthd.conf)"

append_line_if_missing() {
  local file="$1"
  local line="$2"
  if ! grep -Fqx "${line}" "${file}" 2>/dev/null; then
    printf "\n%s\n" "${line}" >> "${file}"
  fi
}

remove_cmdline_prefix() {
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

echo "Installing Plymouth theme payload in rootfs..."
mkdir -p "${THEME_DIR}"
install -m 0644 "${SOURCE_IMAGE}" "${THEME_IMAGE}"

cat > "${PLYMOUTH_DEF}" <<'EOF'
[Plymouth Theme]
Name=El Camino Boot Splash
Description=Static El Camino boot image
ModuleName=script

[script]
ImageDir=/usr/share/plymouth/themes/el-camino
ScriptFile=/usr/share/plymouth/themes/el-camino/el-camino.script
EOF

cat > "${PLYMOUTH_SCRIPT}" <<'EOF'
img = Image("boot_hdmi.png");
sprite = Sprite(img);
sprite.SetZ(10000);
sprite.SetX(0);
sprite.SetY(0);
Plymouth.SetBootProgressFunction(function () {});
Plymouth.SetRefreshFunction(function () {});
EOF

mkdir -p "$(dirname "${DEFAULT_THEME_LINK}")"
ln -sf "${PLYMOUTH_DEF}" "${DEFAULT_THEME_LINK}"

mkdir -p "$(dirname "${PLYMOUTH_CONF}")"
cat > "${PLYMOUTH_CONF}" <<'EOF'
[Daemon]
ShowDelay=0
UseSimpledrm=1
DeviceScale=1
EOF

append_line_if_missing "${CONFIG_TXT}" "disable_splash=1"
append_line_if_missing "${CONFIG_TXT}" "avoid_warnings=1"

remove_cmdline_prefix "${CMDLINE_TXT}" "console=tty1"
for token in \
  "quiet" \
  "splash" \
  "plymouth.ignore-serial-consoles" \
  "vt.global_cursor_default=0" \
  "loglevel=0" \
  "logo.nologo" \
  "rd.systemd.show_status=false" \
  "systemd.show_status=false" \
  "udev.log_priority=0"; do
  ensure_cmdline_token "${CMDLINE_TXT}" "${token}"
done

# Mask getty on tty1 in image filesystem.
mkdir -p "$(in_rootfs /etc/systemd/system)"
ln -sf /dev/null "$(in_rootfs /etc/systemd/system/getty@tty1.service)"
