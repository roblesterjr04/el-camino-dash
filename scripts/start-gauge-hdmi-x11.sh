#!/usr/bin/env bash
set -euo pipefail

exec /usr/bin/xinit /bin/bash -lc \
  "exec runuser -u pi -- /bin/bash -lc 'cd /opt/el-camino-dash; export DISPLAY=:0; export DISPLAY_MODE=hardware; export SDL_VIDEODRIVER=x11; export SDL_AUDIODRIVER=alsa; export SDL_VIDEO_X11_XRANDR=0; export SDL_VIDEO_X11_XINERAMA=0; export SDL_VIDEO_X11_XVIDMODE=0; export PYTHONPATH=/opt/el-camino-dash/src; exec /opt/el-camino-dash/.venv/bin/python /opt/el-camino-dash/src/main.py --config /opt/el-camino-dash/config/dashboard.hdmi5.yaml'" \
  -- /usr/bin/Xorg :0 -nolisten tcp -nocursor vt1
