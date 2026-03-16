#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_rootfs

units=(
  gauge-cluster-hdmi.service
  gauge-hide-cursor.service
  gauge-power-control.service
  gauge-ir-protocol.service
  gauge-cluster-spi.service
)

for unit in "${units[@]}"; do
  install -D -m 0644 \
    "${SOURCE_DIR}/deploy/systemd/${unit}" \
    "$(in_rootfs /etc/systemd/system/${unit})"
done

# Set HDMI units enabled by default in image filesystem.
mkdir -p "$(in_rootfs /etc/systemd/system/graphical.target.wants)"
mkdir -p "$(in_rootfs /etc/systemd/system/multi-user.target.wants)"
ln -sf "../gauge-cluster-hdmi.service" \
  "$(in_rootfs /etc/systemd/system/graphical.target.wants/gauge-cluster-hdmi.service)"
ln -sf "../gauge-hide-cursor.service" \
  "$(in_rootfs /etc/systemd/system/graphical.target.wants/gauge-hide-cursor.service)"
ln -sf "../gauge-power-control.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-power-control.service)"
ln -sf "../gauge-ir-protocol.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-ir-protocol.service)"

# SPI service remains available but disabled/masked by default.
mkdir -p "$(in_rootfs /etc/systemd/system/gauge-cluster-spi.service.d)"
cat > "$(in_rootfs /etc/systemd/system/gauge-cluster-spi.service.d/10-default-disabled.conf)" <<'EOF'
[Unit]
ConditionPathExists=/nonexistent
EOF
