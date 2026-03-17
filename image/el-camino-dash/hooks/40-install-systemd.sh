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
  gauge-connectivity-late.service
)

for unit in "${units[@]}"; do
  install -D -m 0644 \
    "${SOURCE_DIR}/deploy/systemd/${unit}" \
    "$(in_rootfs /etc/systemd/system/${unit})"
done

# Set HDMI units enabled by default in image filesystem.
mkdir -p "$(in_rootfs /etc/systemd/system/multi-user.target.wants)"
ln -sf "../gauge-cluster-hdmi.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-cluster-hdmi.service)"
ln -sf "../gauge-connectivity-late.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-connectivity-late.service)"
ln -sf "../gauge-ir-protocol.service" \
  "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-ir-protocol.service)"
rm -f "$(in_rootfs /etc/systemd/system/graphical.target.wants/gauge-cluster-hdmi.service)"
rm -f "$(in_rootfs /etc/systemd/system/graphical.target.wants/gauge-hide-cursor.service)"
rm -f "$(in_rootfs /etc/systemd/system/graphical.target.wants/gauge-connectivity-late.service)"
rm -f "$(in_rootfs /etc/systemd/system/multi-user.target.wants/gauge-power-control.service)"
rm -f "$(in_rootfs /etc/systemd/system/default.target.wants/gauge-power-control.service)"
rm -f "$(in_rootfs /etc/systemd/system/multi-user.target.wants/bluetooth.service)"
rm -f "$(in_rootfs /etc/systemd/system/bluetooth.target.wants/bluetooth.service)"
rm -f "$(in_rootfs /etc/systemd/system/graphical.target.wants/lightdm.service)"
rm -f "$(in_rootfs /etc/systemd/system/display-manager.service)"

# Keep power-control unit installed but disabled by default to avoid unintended
# shutdown loops from noisy/miswired control lines during bring-up.
mkdir -p "$(in_rootfs /etc/systemd/system/gauge-power-control.service.d)"
cat > "$(in_rootfs /etc/systemd/system/gauge-power-control.service.d/10-default-disabled.conf)" <<'EOF'
[Unit]
ConditionPathExists=/etc/elc-enable-power-control
EOF

# SPI service remains available but disabled/masked by default.
mkdir -p "$(in_rootfs /etc/systemd/system/gauge-cluster-spi.service.d)"
cat > "$(in_rootfs /etc/systemd/system/gauge-cluster-spi.service.d/10-default-disabled.conf)" <<'EOF'
[Unit]
ConditionPathExists=/nonexistent
EOF
