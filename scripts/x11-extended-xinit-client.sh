#!/usr/bin/env bash
# Run as root under xinit (before python). Sets RandR layout, then drops to pi for pygame.
set -euo pipefail
RUNROOT="/opt/el-camino-dash"
export DISPLAY="${DISPLAY:-:0}"
export DISPLAY_MODE=hardware
sleep 1
if [[ -x "${RUNROOT}/scripts/x11-configure-extended-desktop.sh" ]]; then
  "${RUNROOT}/scripts/x11-configure-extended-desktop.sh" || true
fi
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
