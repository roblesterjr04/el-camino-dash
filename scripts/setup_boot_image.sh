#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo bash scripts/setup_boot_image.sh" >&2
  exit 1
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_IMAGE="${PROJECT_DIR}/assets/bootscreen/boot.png"

if [[ ! -f "${SOURCE_IMAGE}" ]]; then
  echo "Boot image not found: ${SOURCE_IMAGE}" >&2
  exit 1
fi

if [[ -f /boot/firmware/config.txt ]]; then
  BOOT_DIR="/boot/firmware"
elif [[ -f /boot/config.txt ]]; then
  BOOT_DIR="/boot"
else
  echo "Could not locate Raspberry Pi boot config directory." >&2
  exit 1
fi

CONFIG_TXT="${BOOT_DIR}/config.txt"
CMDLINE_TXT="${BOOT_DIR}/cmdline.txt"
THEME_DIR="/usr/share/plymouth/themes/el-camino"
PLYMOUTH_DEF="/usr/share/plymouth/themes/el-camino/el-camino.plymouth"
PLYMOUTH_SCRIPT="/usr/share/plymouth/themes/el-camino/el-camino.script"
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

set_cmdline() {
  local file="$1"
  shift
  local cmdline
  cmdline="$(<"${file}")"
  for token in "$@"; do
    if [[ "${cmdline}" != *"${token}"* ]]; then
      cmdline="${cmdline} ${token}"
      BOOT_CHANGED=1
    fi
  done
  printf "%s\n" "${cmdline}" > "${file}"
}

remove_cmdline_prefix() {
  local file="$1"
  local prefix="$2"
  local cmdline
  cmdline="$(<"${file}")"
  local rebuilt=""
  local changed=0
  for token in ${cmdline}; do
    if [[ "${token}" == "${prefix}"* ]]; then
      changed=1
      continue
    fi
    rebuilt="${rebuilt} ${token}"
  done
  rebuilt="${rebuilt# }"
  if [[ "${changed}" -eq 1 ]]; then
    printf "%s\n" "${rebuilt}" > "${file}"
    BOOT_CHANGED=1
  fi
}

echo "[1/6] Installing Plymouth..."
apt-get update
apt-get install -y plymouth

echo "[2/6] Installing custom Plymouth theme..."
mkdir -p "${THEME_DIR}"
cp "${SOURCE_IMAGE}" "${THEME_DIR}/boot.png"

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
img = Image("boot.png");
sprite = Sprite(img);
sprite.SetZ(10000);

fun center_image () {
  w = Window.GetWidth();
  h = Window.GetHeight();
  iw = img.GetWidth();
  ih = img.GetHeight();
  sprite.SetX((w - iw) / 2);
  sprite.SetY((h - ih) / 2);
}

center_image();

Plymouth.SetBootProgressFunction(function () {});
Plymouth.SetRefreshFunction(function () { center_image(); });
EOF

echo "[3/6] Setting default splash theme..."
if command -v plymouth-set-default-theme >/dev/null 2>&1; then
  plymouth-set-default-theme el-camino
fi
if command -v update-initramfs >/dev/null 2>&1; then
  update-initramfs -u
fi

echo "[4/6] Updating boot config..."
append_line_if_missing "${CONFIG_TXT}" "disable_splash=1"
append_line_if_missing "${CONFIG_TXT}" "avoid_warnings=1"

echo "[5/6] Updating kernel cmdline for quiet splash..."
remove_cmdline_prefix "${CMDLINE_TXT}" "console=tty1"
set_cmdline "${CMDLINE_TXT}" \
  "quiet" \
  "splash" \
  "plymouth.ignore-serial-consoles" \
  "vt.global_cursor_default=0" \
  "loglevel=0" \
  "logo.nologo" \
  "rd.systemd.show_status=false" \
  "systemd.show_status=false" \
  "udev.log_priority=0"

echo "[6/6] Disabling visible TTY1 login prompt..."
systemctl disable getty@tty1.service >/dev/null 2>&1 || true
systemctl mask getty@tty1.service >/dev/null 2>&1 || true

echo
echo "Boot splash setup complete."
echo "Boot image: ${SOURCE_IMAGE}"
echo "Theme: el-camino"
if [[ "${BOOT_CHANGED}" -eq 1 ]]; then
  echo "Boot configuration changed. Reboot required:"
  echo "  sudo reboot"
else
  echo "No boot config changes detected."
  echo "Reboot to verify splash behavior:"
  echo "  sudo reboot"
fi
