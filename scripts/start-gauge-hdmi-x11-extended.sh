#!/usr/bin/env bash
# Single pygame window on an X11 "extended desktop" spanning two HDMI outputs (RandR).
# Use when: splitter + 4in are two separate DRM outputs; config uses one logical canvas (e.g. 3600×1080).
# Does not use KMSDRM — requires Xorg (this script starts it via xinit).
set -euo pipefail

RUNROOT="/opt/el-camino-dash"
CFG="${RUNROOT}/config/dashboard.hdmi5.yaml"
XINIT_HELPER="${RUNROOT}/scripts/x11-configure-extended-desktop.sh"

exec /usr/bin/xinit /bin/bash -lc \
  "exec runuser -u pi -- /bin/bash -lc '
    cd \"${RUNROOT}\"
    export DISPLAY=:0
    export DISPLAY_MODE=hardware
    export SDL_VIDEODRIVER=x11
    export SDL_AUDIODRIVER=alsa
    # Allow SDL to see the full RandR virtual desktop (multi-monitor span)
    export SDL_VIDEO_X11_XRANDR=1
    export PYTHONPATH=\"${RUNROOT}/src\"
    sleep 1
    if [[ -x \"${XINIT_HELPER}\" ]]; then
      \"${XINIT_HELPER}\" || true
    fi
    exec \"${RUNROOT}/.venv/bin/python\" \"${RUNROOT}/src/main.py\" --config \"${CFG}\"
  '" \
  -- /usr/bin/Xorg :0 -nolisten tcp -nocursor vt1
