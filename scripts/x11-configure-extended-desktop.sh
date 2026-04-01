#!/usr/bin/env bash
# Place two X11 outputs side-by-side (extended desktop) so one pygame window can span both.
# KMSDRM cannot do this; use with start-gauge-hdmi-x11-extended.sh (SDL_VIDEODRIVER=x11).
#
# Output names: run `DISPLAY=:0 xrandr` on the Pi (e.g. HDMI-1, HDMI-2, XWAYLAND may differ).
# Optional env:
#   ELC_X11_OUTPUT_LEFT   — connector that should be on the left (wide / splitter)
#   ELC_X11_OUTPUT_RIGHT  — connector on the right (e.g. 4in); if unset, picks the other connected output

set -euo pipefail
: "${DISPLAY:=:0}"
export DISPLAY

if ! command -v xrandr >/dev/null 2>&1; then
  echo "[elc-x11] xrandr not installed; skipping extended layout" >&2
  exit 0
fi

mapfile -t CONNECTED < <(xrandr 2>/dev/null | awk '/ connected/{print $1}')
if [[ ${#CONNECTED[@]} -lt 2 ]]; then
  echo "[elc-x11] need two connected outputs; got ${#CONNECTED[@]}: ${CONNECTED[*]}" >&2
  exit 0
fi

LEFT="${ELC_X11_OUTPUT_LEFT:-}"
RIGHT="${ELC_X11_OUTPUT_RIGHT:-}"

if [[ -n "$LEFT" && -n "$RIGHT" ]]; then
  xrandr --output "$LEFT" --auto --pos 0x0 --output "$RIGHT" --auto --right-of "$LEFT"
  echo "[elc-x11] xrandr: $LEFT | $RIGHT (explicit)"
  exit 0
fi

if [[ -z "$LEFT" && -z "$RIGHT" ]]; then
  xrandr --output "${CONNECTED[0]}" --auto --pos 0x0 --output "${CONNECTED[1]}" --auto --right-of "${CONNECTED[0]}"
  echo "[elc-x11] xrandr: ${CONNECTED[0]} | ${CONNECTED[1]} (auto order — set ELC_X11_OUTPUT_LEFT/RIGHT if wrong)"
  exit 0
fi

echo "[elc-x11] set both ELC_X11_OUTPUT_LEFT and ELC_X11_OUTPUT_RIGHT, or neither" >&2
exit 1
