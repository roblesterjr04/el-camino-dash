#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

require_rootfs

# No display manager/window manager session setup.
# Dashboard owns HDMI directly via SDL KMSDRM from gauge-cluster-hdmi.service.
rm -rf "$(in_rootfs /etc/lightdm/lightdm.conf.d)" 2>/dev/null || true
rm -f "$(in_rootfs /home/${TARGET_USER}/.xsession)" 2>/dev/null || true
rm -f "$(in_rootfs /etc/systemd/system/display-manager.service)" 2>/dev/null || true
