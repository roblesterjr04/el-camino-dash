#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo bash scripts/install_runtime_services.sh" >&2
  exit 1
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSTEMD_DIR="${PROJECT_DIR}/deploy/systemd"

if [[ ! -d "${SYSTEMD_DIR}" ]]; then
  echo "Missing systemd directory: ${SYSTEMD_DIR}" >&2
  exit 1
fi

units=(
  gauge-cluster-hdmi.service
  gauge-hide-cursor.service
  gauge-power-control.service
  gauge-ir-protocol.service
  gauge-cluster-spi.service
)

for unit in "${units[@]}"; do
  install -m 0644 "${SYSTEMD_DIR}/${unit}" "/etc/systemd/system/${unit}"
done

install -d /etc/systemd/system/gauge-cluster-spi.service.d
cat > /etc/systemd/system/gauge-cluster-spi.service.d/10-default-disabled.conf <<'EOF'
[Unit]
ConditionPathExists=/nonexistent
EOF

systemctl daemon-reload
systemctl set-default graphical.target || true
systemctl disable gauge-cluster-spi.service >/dev/null 2>&1 || true
systemctl mask gauge-cluster-spi.service >/dev/null 2>&1 || true
systemctl enable gauge-cluster-hdmi.service
systemctl enable gauge-hide-cursor.service
systemctl enable gauge-power-control.service
systemctl enable gauge-ir-protocol.service

# Ensure the runtime user can open /dev/input/event* (IR via evdev).
if id pi >/dev/null 2>&1; then
  for grp in input gpio i2c audio bluetooth dialout; do
    if getent group "${grp}" >/dev/null 2>&1; then
      usermod -aG "${grp}" pi >/dev/null 2>&1 || true
    fi
  done
fi
