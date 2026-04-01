#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_rootfs

units=(
  elcamino-voice.service
  gauge-expand-rootfs.service
  gauge-cluster-hdmi.service
  gauge-cluster-hdmi-x11-extended.service
  gauge-power-control.service
  gauge-ir-protocol.service
  gauge-wifi-diagnostics.service
  gauge-ssh-diagnostics.service
)

for unit in "${units[@]}"; do
  install -D -m 0644 \
    "${SOURCE_DIR}/deploy/systemd/${unit}" \
    "$(in_rootfs /etc/systemd/system/${unit})"
done

# Set HDMI units enabled by default in image filesystem.
mkdir -p "$(in_rootfs /etc/systemd/system/multi-user.target.wants)"
ln -sf "../gauge-expand-rootfs.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-expand-rootfs.service)"
ln -sf "../gauge-cluster-hdmi.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-cluster-hdmi.service)"
ln -sf "../gauge-ir-protocol.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-ir-protocol.service)"
ln -sf "../gauge-power-control.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-power-control.service)"
ln -sf "../gauge-wifi-diagnostics.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-wifi-diagnostics.service)"
ln -sf "../gauge-ssh-diagnostics.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-ssh-diagnostics.service)"
rm -f "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-xorg.service)"
rm -f "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-hide-cursor.service)"
rm -f "$(in_rootfs /etc/systemd/system/graphical.target.wants/gauge-cluster-hdmi.service)"
rm -f "$(in_rootfs /etc/systemd/system/graphical.target.wants/gauge-hide-cursor.service)"
rm -f "$(in_rootfs /etc/systemd/system/default.target.wants/gauge-power-control.service)"
rm -f "$(in_rootfs /etc/systemd/system/graphical.target.wants/lightdm.service)"
rm -f "$(in_rootfs /etc/systemd/system/display-manager.service)"
