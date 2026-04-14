#!/usr/bin/env bash
# Run as root under xinit (before python). Sets RandR layout, then drops to pi for pygame.
set -euo pipefail
RUNROOT="/opt/el-camino-dash"
export DISPLAY="${DISPLAY:-:0}"
export DISPLAY_MODE=hardware
# side_by_side: 2160-wide splitter + 720 round as one X screen (2880×1080); multi_window tiles gauges.
# overlap: legacy single pygame window spanning both outputs.
export ELC_X11_LAYOUT="${ELC_X11_LAYOUT:-side_by_side}"
sleep 1
if [[ -x "${RUNROOT}/scripts/x11-configure-extended-desktop.sh" ]]; then
  "${RUNROOT}/scripts/x11-configure-extended-desktop.sh" || true
fi
# Let RandR/modes settle before SDL opens the display (avoids intermittent "No available displays").
sleep 2
# Block until the X server answers (pygame often races RandR and exits with "No available displays").
if command -v xdpyinfo >/dev/null 2>&1; then
  for _ in $(seq 1 120); do
    if DISPLAY=:0 xdpyinfo >/dev/null 2>&1; then
      break
    fi
    sleep 0.25
  done
fi
sleep 0.5
exec runuser -u pi -- /bin/bash -lc "
  cd \"${RUNROOT}\"
  export DISPLAY=:0
  export DISPLAY_MODE=hardware
  export SDL_VIDEODRIVER=x11
  export SDL_AUDIODRIVER=alsa
  export SDL_VIDEO_X11_XRANDR=1
  export PYTHONPATH=\"${RUNROOT}/src\"
  exec \"${RUNROOT}/.venv/bin/python\" \"${RUNROOT}/src/main.py\" --config \"${RUNROOT}/config/dashboard.hdmi5.yaml\"
"
