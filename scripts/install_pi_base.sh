#!/usr/bin/env bash
set -euo pipefail

DRY_RUN=0
for arg in "$@"; do
  case "$arg" in
    --dry-run)
      DRY_RUN=1
      ;;
  esac
done

run_cmd() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[dry-run] $*"
  else
    eval "$@"
  fi
}

require_root() {
  if [[ "${EUID}" -ne 0 ]]; then
    echo "This script must run as root (use sudo)." >&2
    exit 1
  fi
}

require_root

run_cmd "apt-get update"
run_cmd "apt-get install -y python3 python3-venv python3-pip python3-dev build-essential git"
run_cmd "apt-get install -y libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libsdl2-ttf-dev libportmidi-dev"
run_cmd "apt-get install -y navit bluez pulseaudio"
run_cmd "apt-get install -y raspi-config python3-rpi.gpio python3-spidev"

if [[ -f /boot/firmware/config.txt ]]; then
  cfg="/boot/firmware/config.txt"
else
  cfg="/boot/config.txt"
fi

append_if_missing() {
  local line="$1"
  if grep -Fq "$line" "$cfg"; then
    echo "Already set: $line"
  else
    run_cmd "printf '\n%s\n' '$line' >> '$cfg'"
  fi
}

append_if_missing "dtparam=spi=on"
append_if_missing "dtparam=i2c_arm=on"

echo "Base install complete. Reboot recommended before hardware verification."
