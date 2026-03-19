#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_rootfs

PACKAGES=(
  python3-pip python3-venv
  python3-pygame python3-pil python3-spidev python3-rpi.gpio python3-evdev
  ir-keytable i2c-tools alsa-utils raspi-config
  gcc-avr binutils-avr avr-libc
  rsync xinit xserver-xorg xauth
  bluez pulseaudio pulseaudio-module-bluetooth
  navit gpsd gpsd-clients
  plymouth plymouth-themes
  curl ca-certificates avrdude
)

manifest="$(in_rootfs /opt/el-camino-dash/.image-packages.txt)"
mkdir -p "$(dirname "${manifest}")"
printf "%s\n" "${PACKAGES[@]}" > "${manifest}"

# Some image build systems expose direct apt hooks; others do package install
# outside custom hooks. We keep a canonical package manifest in the image and
# optionally install if a helper command is exposed.
if command -v mmdebstrap-install >/dev/null 2>&1; then
  mmdebstrap-install "${PACKAGES[@]}"
else
  chroot "${ROOTFS_DIR}" /usr/bin/apt-get update
  chroot "${ROOTFS_DIR}" /usr/bin/apt-get install -y --no-install-recommends "${PACKAGES[@]}"
fi

# Ensure runtime user group memberships are present in image state.
for grp in bluetooth audio i2c gpio dialout input; do
  if chroot "${ROOTFS_DIR}" getent group "${grp}" >/dev/null 2>&1; then
    chroot "${ROOTFS_DIR}" usermod -aG "${grp}" "${TARGET_USER}" >/dev/null 2>&1 || true
  fi
done
