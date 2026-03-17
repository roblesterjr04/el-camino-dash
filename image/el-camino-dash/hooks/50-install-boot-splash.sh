#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_rootfs

THEME_NAME="digidash"
SOURCE_IMAGE="${SOURCE_DIR}/assets/bootscreen/boot.png"
SOURCE_THEMES_DIR="${SOURCE_DIR}/image/el-camino-dash/image/mbr/simple_dual/plymouth/themes"
SOURCE_THEME_DIR="${SOURCE_THEMES_DIR}/${THEME_NAME}"
if [[ ! -d "${SOURCE_THEME_DIR}" ]]; then
  echo "Skipping boot splash hook: theme directory missing (${SOURCE_THEME_DIR})." >&2
  exit 0
fi
if [[ ! -f "${SOURCE_IMAGE}" ]]; then
  echo "Skipping boot splash hook: source image missing (${SOURCE_IMAGE})." >&2
  exit 0
fi
if [[ ! -f "${SOURCE_THEME_DIR}/${THEME_NAME}.plymouth" || ! -f "${SOURCE_THEME_DIR}/${THEME_NAME}.script" ]]; then
  echo "Skipping boot splash hook: missing ${THEME_NAME}.plymouth or ${THEME_NAME}.script." >&2
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
THEMES_ROOT="$(in_rootfs /usr/share/plymouth/themes)"

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
mkdir -p "${THEMES_ROOT}"
rsync -a --exclude ".DS_Store" "${SOURCE_THEMES_DIR}/" "${THEMES_ROOT}/"
install -m 0644 "${SOURCE_IMAGE}" "${THEMES_ROOT}/${THEME_NAME}/boot.png"

# Activate script splash via plymouth tooling (same pattern as reference repo).
chroot "${ROOTFS_DIR}" /usr/sbin/plymouth-set-default-theme "${THEME_NAME}" || true

PLYMOUTH_CONF="$(in_rootfs /etc/plymouth/plymouthd.conf)"
mkdir -p "$(dirname "${PLYMOUTH_CONF}")"
if [[ -f "${PLYMOUTH_CONF}" ]]; then
  if grep -Eq '^[[:space:]]*Theme=' "${PLYMOUTH_CONF}"; then
    sed -i -E "s|^[[:space:]]*Theme=.*$|Theme=${THEME_NAME}|" "${PLYMOUTH_CONF}"
  else
    printf "\nTheme=%s\n" "${THEME_NAME}" >> "${PLYMOUTH_CONF}"
  fi
else
  cat > "${PLYMOUTH_CONF}" <<EOF
[Daemon]
Theme=${THEME_NAME}
EOF
fi

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
