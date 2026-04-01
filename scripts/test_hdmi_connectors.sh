#!/usr/bin/env bash
# Quick DRM / sysfs snapshot for debugging a second HDMI (or any connector).
# Run on the Pi: bash /opt/el-camino-dash/scripts/test_hdmi_connectors.sh
set -euo pipefail

echo "=== $(date -Is) ==="
echo "=== /dev/dri ==="
ls -la /dev/dri/ 2>/dev/null || echo "(no /dev/dri)"
echo "  (On many Pi boards, card0=v3d has no connectors; HDMI is /dev/dri/card1.)"

echo
echo "=== DRM connector status / modes (HDMI*) ==="
shopt -s nullglob
for d in /sys/class/drm/card*-HDMI-*; do
  base=$(basename "$d")
  st="${d}/status"
  en="${d}/enabled"
  mo="${d}/modes"
  echo "--- $base ---"
  [[ -f "$st" ]] && echo "  status:   $(cat "$st")"
  [[ -f "$en" ]] && echo "  enabled:  $(cat "$en")"
  if [[ -f "$mo" ]]; then
    echo "  modes:"
    sed 's/^/    /' "$mo" | head -20
  fi
done
shopt -u nullglob

echo
echo "=== modetest -c (Debian: apt install -y drm-utils) ==="
DRM_DEV="/dev/dri/card1"
[[ -e "$DRM_DEV" ]] || DRM_DEV="/dev/dri/card0"
if command -v modetest >/dev/null 2>&1; then
  modetest -D "$DRM_DEV" -c 2>/dev/null | head -120 || true
else
  echo "  (modetest not installed — try: sudo apt install -y drm-utils)"
fi

echo
echo "=== xrandr (only if X is running; e.g. DISPLAY=:0) ==="
if [[ -n "${DISPLAY:-}" ]] && command -v xrandr >/dev/null 2>&1; then
  xrandr 2>/dev/null || true
else
  echo "  No DISPLAY or xrandr — KMS-only session (normal for gauge-cluster-hdmi.service)."
fi

echo
echo "=== hint ==="
echo "  Second HDMI often stays blank until something opens that output."
echo "  Options: (1) drm-utils (modetest); (2) scripts/test_hdmi_kmscube.sh; (3) X11 + scripts/test_hdmi_pygame_x11.py"
