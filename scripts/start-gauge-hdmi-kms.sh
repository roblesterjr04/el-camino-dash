#!/usr/bin/env bash
set -euo pipefail

cd /opt/el-camino-dash
export DISPLAY_MODE=auto
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-kmsdrm}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
export PYTHONPATH=/opt/el-camino-dash/src

exec /opt/el-camino-dash/.venv/bin/python \
  /opt/el-camino-dash/src/main.py \
  --config /opt/el-camino-dash/config/dashboard.hdmi5.yaml
