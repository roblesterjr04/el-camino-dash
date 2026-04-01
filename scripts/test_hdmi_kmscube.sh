#!/usr/bin/env bash
# Animated GL on DRM (usually both connectors get activity). Requires kmscube:
#   sudo apt install -y kmscube
#
# The dashboard holds DRM master — stop it first or this may fail / show nothing:
#   sudo systemctl stop gauge-cluster-hdmi.service
#
# Run (8s then exit):
#   bash /opt/el-camino-dash/scripts/test_hdmi_kmscube.sh

set -euo pipefail
if ! command -v kmscube >/dev/null 2>&1; then
  echo "Install: sudo apt install -y kmscube" >&2
  exit 1
fi
echo "[elc-test] running kmscube for 8s (Ctrl+C to stop early)..." >&2
exec timeout 8s kmscube
