#!/usr/bin/env bash
# Place two X11 outputs so one pygame window can span both (extended / overlapping).
# KMSDRM cannot do this; use with start-gauge-hdmi-x11-extended.sh (SDL_VIDEODRIVER=x11).
#
# Output names: run `DISPLAY=:0 xrandr` on the Pi (e.g. HDMI-1, HDMI-2).
# Optional env:
#   ELC_X11_OUTPUT_LEFT   — connector that should be the wide / splitter output
#   ELC_X11_OUTPUT_RIGHT  — the other connector (e.g. 4in)
#   ELC_X11_LAYOUT        — overlap | side_by_side (default: auto — overlap when wide+small detected)
#   ELC_X11_OVERLAP_POS   — X for the 4″ in overlap mode (default: auto from Screen width vs 2880 canvas — 2160→810, 2880→1080)
#   ELC_X11_LAYOUT_CANVAS_WIDTH — logical triple-wide width for that formula (default 2880)
#   ELC_X11_ROTATE        — none | inverted | left | right | normal (default none — inverted+overlap can break SDL on Pi)
#   ELC_X11_CONNECT_WAIT_SEC — seconds to wait for a second HDMI to enumerate (default 20)
#   ELC_X11_FORCE_FOUR_INCH — connector name for the 4″ (e.g. HDMI-2) if 720×720 detection picks the wrong port
#   ELC_X11_ROTATE_NARROW — none | inverted | left | right — RandR rotate for the small (720) output only in side_by_side (if pygame view_rotation_deg is not enough)

set -euo pipefail
: "${DISPLAY:=:0}"
export DISPLAY

ELC_X11_ROTATE="${ELC_X11_ROTATE:-none}"
XR_ROT=()
case "${ELC_X11_ROTATE}" in
  inverted|180) XR_ROT=(--rotate inverted) ;;
  left) XR_ROT=(--rotate left) ;;
  right) XR_ROT=(--rotate right) ;;
  normal) XR_ROT=(--rotate normal) ;;
  none|0|'') XR_ROT=() ;;
  *)
    echo "[elc-x11] unknown ELC_X11_ROTATE=${ELC_X11_ROTATE}; skipping xrandr rotation" >&2
    XR_ROT=()
    ;;
esac

NARROW_ROT=()
case "${ELC_X11_ROTATE_NARROW:-none}" in
  inverted|180) NARROW_ROT=(--rotate inverted) ;;
  left) NARROW_ROT=(--rotate left) ;;
  right) NARROW_ROT=(--rotate right) ;;
  normal) NARROW_ROT=(--rotate normal) ;;
  none|0|'') NARROW_ROT=() ;;
  *)
    echo "[elc-x11] unknown ELC_X11_ROTATE_NARROW=${ELC_X11_ROTATE_NARROW}; skipping narrow rotation" >&2
    NARROW_ROT=()
    ;;
esac

if ! command -v xrandr >/dev/null 2>&1; then
  echo "[elc-x11] xrandr not installed; skipping extended layout" >&2
  exit 0
fi

get_width() {
  local name="$1"
  local line
  line=$(xrandr 2>/dev/null | grep "^${name} connected" || true)
  if [[ "$line" =~ ([0-9]+)x([0-9]+)\+ ]]; then
    echo "${BASH_REMATCH[1]}"
    return
  fi
  if [[ "$line" =~ ([0-9]+)x([0-9]+) ]]; then
    echo "${BASH_REMATCH[1]}"
    return
  fi
  echo 0
}

# Mode lines listed under an output (first column, e.g. 2880x1080, 720x720_60.00).
list_modes_for_output() {
  local out="$1"
  xrandr 2>/dev/null | awk -v o="$out" '
    $0 ~ "^"o" connected" { p=1; next }
    p && /^[A-Za-z0-9].* connected/ { exit }
    p && /^[[:space:]]+[0-9]+x[0-9]+/ { sub(/^[[:space:]]+/, ""); print $1 }
  '
}

pick_wide_mode_name() {
  local out="$1"
  local m
  while read -r m; do
    case "$m" in
      2880x1080|2880x1080_60.00) echo "$m"; return 0 ;;
    esac
  done < <(list_modes_for_output "$out")
  while read -r m; do
    case "$m" in
      2160x1080|2160x1080_60.00) echo "$m"; return 0 ;;
    esac
  done < <(list_modes_for_output "$out")
  while read -r m; do
    w="${m%%x*}"
    if [[ "$w" =~ ^[0-9]+$ ]] && [[ "$w" -ge 2000 ]]; then
      echo "$m"
      return 0
    fi
  done < <(list_modes_for_output "$out")
  echo ""
}

pick_narrow_mode_name() {
  local out="$1"
  local m
  while read -r m; do
    case "$m" in
      720x720|720x720_60.00) echo "$m"; return 0 ;;
    esac
  done < <(list_modes_for_output "$out")
  echo "720x720"
}

# 4″ round panel is identified by a 720x720 mode from config.txt [HDMI:1] hdmi_cvt, not by current pixel clock.
infer_wide_and_narrow_output() {
  EL_INFERRED_WIDE=""
  EL_INFERRED_NARROW=""
  local o wa wb
  for o in "${CONNECTED[@]}"; do
    if list_modes_for_output "$o" | grep -qE '^720x720'; then
      EL_INFERRED_NARROW="$o"
      break
    fi
  done
  for o in "${CONNECTED[@]}"; do
    if [[ -n "$EL_INFERRED_NARROW" && "$o" == "$EL_INFERRED_NARROW" ]]; then
      continue
    fi
    EL_INFERRED_WIDE="$o"
    break
  done
  if [[ -z "$EL_INFERRED_NARROW" && ${#CONNECTED[@]} -ge 2 ]]; then
    wa=$(get_width "${CONNECTED[0]}")
    wb=$(get_width "${CONNECTED[1]}")
    if [[ "$wa" -le 800 ]] && [[ "$wb" -ge 1000 ]]; then
      EL_INFERRED_NARROW="${CONNECTED[0]}"
      EL_INFERRED_WIDE="${CONNECTED[1]}"
    elif [[ "$wb" -le 800 ]] && [[ "$wa" -ge 1000 ]]; then
      EL_INFERRED_NARROW="${CONNECTED[1]}"
      EL_INFERRED_WIDE="${CONNECTED[0]}"
    fi
  fi
}

ELC_X11_CONNECT_WAIT_SEC="${ELC_X11_CONNECT_WAIT_SEC:-20}"
CONNECTED=()
for ((wait_i = 0; wait_i < ELC_X11_CONNECT_WAIT_SEC; wait_i++)); do
  mapfile -t CONNECTED < <(xrandr 2>/dev/null | awk '/ connected/{print $1}')
  if [[ ${#CONNECTED[@]} -ge 2 ]]; then
    break
  fi
  # One HDMI: do not stall the whole boot waiting for a cable that is not there.
  if [[ ${#CONNECTED[@]} -eq 1 && "$wait_i" -ge 2 ]]; then
    break
  fi
  sleep 1
done
[[ ${#CONNECTED[@]} -eq 0 ]] && mapfile -t CONNECTED < <(xrandr 2>/dev/null | awk '/ connected/{print $1}')
# HDMI can enumerate after Xorg starts; keep probing before giving up (avoids SDL "No available displays").
if [[ ${#CONNECTED[@]} -eq 0 ]]; then
  for ((z = 0; z < 30; z++)); do
    sleep 1
    mapfile -t CONNECTED < <(xrandr 2>/dev/null | awk '/ connected/{print $1}')
    [[ ${#CONNECTED[@]} -gt 0 ]] && break
  done
fi

if [[ ${#CONNECTED[@]} -lt 1 ]]; then
  echo "[elc-x11] no connected outputs after wait" >&2
  exit 0
fi
if [[ ${#CONNECTED[@]} -eq 1 ]]; then
  o="${CONNECTED[0]}"
  if xrandr --output "$o" --auto "${XR_ROT[@]}" 2>/dev/null; then
    echo "[elc-x11] single output $o rotate=${ELC_X11_ROTATE}"
  else
    xrandr --output "$o" --auto 2>/dev/null || true
    echo "[elc-x11] single output $o (rotate failed; using auto only)" >&2
  fi
  exit 0
fi

# Apply modes before parsing widths — otherwise get_width can be 0 and we fall back to side-by-side
# (4″ ends at +2160 while the app draws the center strip at x=1080 → black 4″).
for o in "${CONNECTED[@]}"; do
  xrandr --output "$o" --auto 2>/dev/null || true
done
sleep 0.4
for o in "${CONNECTED[@]}"; do
  xrandr --output "$o" --auto 2>/dev/null || true
done

infer_wide_and_narrow_output
if [[ -n "${ELC_X11_FORCE_FOUR_INCH:-}" ]]; then
  EL_INFERRED_NARROW="${ELC_X11_FORCE_FOUR_INCH}"
  EL_INFERRED_WIDE=""
  for o in "${CONNECTED[@]}"; do
    if [[ "$o" == "$EL_INFERRED_NARROW" ]]; then
      continue
    fi
    EL_INFERRED_WIDE="$o"
    break
  done
  echo "[elc-x11] using ELC_X11_FORCE_FOUR_INCH=${EL_INFERRED_NARROW} wide=${EL_INFERRED_WIDE}" >&2
fi
if [[ -n "${EL_INFERRED_NARROW:-}" && -n "${EL_INFERRED_WIDE:-}" ]]; then
  echo "[elc-x11] inferred 4″ (720×720 modes)=${EL_INFERRED_NARROW} wide/splitter=${EL_INFERRED_WIDE}" >&2
fi

LEFT="${ELC_X11_OUTPUT_LEFT:-}"
RIGHT="${ELC_X11_OUTPUT_RIGHT:-}"
LAYOUT="${ELC_X11_LAYOUT:-auto}"

ensure_narrow_has_mode() {
  local narrow="$1"
  local pos="$2"
  if xrandr 2>/dev/null | grep -A25 "^${narrow} connected" | grep -q '\*'; then
    return 0
  fi
  echo "[elc-x11] ${narrow} had no *active mode; forcing 720x720 @${pos}x0" >&2
  xrandr --output "$narrow" --mode 720x720 --pos "${pos}x0" 2>/dev/null && return 0
  xrandr --output "$narrow" --auto --pos "${pos}x0" 2>/dev/null || true
}

overlap_warn_if_screen_narrow() {
  local sw
  sw=$(xrandr 2>/dev/null | sed -n 's/.*current \([0-9][0-9]*\) x \([0-9][0-9]*\).*/\1/p' | head -1)
  if [[ -n "${sw}" ]] && [[ "${sw}" -lt 2880 ]]; then
    echo "[elc-x11] WARNING: Screen width is ${sw}px (not 2880). If the 4″ is wrong, check hdmi_cvt + [HDMI:n] port order in /boot/firmware/config.txt." >&2
  fi
}

# Match pygame canvas_scale_to_desktop_width: center strip starts at 1080 in 2880px design; scale by the *wide* output width.
# Do not use Screen0 total width — side-by-side (2160+720) reports 2880 and would wrongly yield overlap X=1080 instead of 810.
overlap_pos_for_triple_canvas() {
  local wide_out="$1"
  local ref sw pos
  ref="${ELC_X11_LAYOUT_CANVAS_WIDTH:-2880}"
  if [[ -n "${ELC_X11_OVERLAP_POS:-}" ]]; then
    echo "${ELC_X11_OVERLAP_POS}"
    return
  fi
  sw=$(get_width "$wide_out")
  if [[ -z "$sw" || "$sw" -lt 1 ]]; then
    sw=$(xrandr 2>/dev/null | sed -n 's/.*current \([0-9][0-9]*\) x \([0-9][0-9]*\).*/\1/p' | head -1)
  fi
  if [[ -z "$sw" || "$sw" -lt 1 ]]; then
    echo "1080"
    return
  fi
  pos=$((1080 * sw / ref))
  echo "$pos"
}

run_overlap() {
  local wide="$1"
  local narrow="$2"
  local wm nm pos
  pos="$(overlap_pos_for_triple_canvas "$wide")"
  if [[ -z "${ELC_X11_OVERLAP_POS:-}" ]]; then
    echo "[elc-x11] overlap X=${pos}px (1080×wide_width÷${ELC_X11_LAYOUT_CANVAS_WIDTH:-2880}; matches pygame canvas_scale; override with ELC_X11_OVERLAP_POS)" >&2
  fi
  wm=$(pick_wide_mode_name "$wide")
  nm=$(pick_narrow_mode_name "$narrow")
  xrandr --fb 2880x1080 2>/dev/null || true
  if [[ -n "$wm" && -n "$nm" ]]; then
    if xrandr --output "$wide" --mode "$wm" "${XR_ROT[@]}" --pos 0x0 \
        --output "$narrow" --mode "$nm" "${XR_ROT[@]}" --pos "${pos}x0" 2>/dev/null; then
      ensure_narrow_has_mode "$narrow" "$pos"
      echo "[elc-x11] xrandr: $wide mode=${wm} @0x0 + $narrow mode=${nm} @${pos}x0 (overlap / explicit modes) rotate=${ELC_X11_ROTATE}"
      overlap_warn_if_screen_narrow
      return 0
    fi
    xrandr --fb 2880x1080 2>/dev/null || true
    if xrandr --output "$wide" --mode "$wm" "${XR_ROT[@]}" --pos 0x0 2>/dev/null \
      && xrandr --output "$narrow" --mode "$nm" "${XR_ROT[@]}" --pos "${pos}x0" 2>/dev/null; then
      ensure_narrow_has_mode "$narrow" "$pos"
      echo "[elc-x11] xrandr: $wide mode=${wm} @0x0 + $narrow mode=${nm} @${pos}x0 (overlap / explicit staged) rotate=${ELC_X11_ROTATE}"
      overlap_warn_if_screen_narrow
      return 0
    fi
    echo "[elc-x11] explicit modes failed (wide=${wm} narrow=${nm}); trying --auto" >&2
  fi
  if xrandr --output "$wide" --auto "${XR_ROT[@]}" --pos 0x0 --output "$narrow" --auto "${XR_ROT[@]}" --pos "${pos}x0" 2>/dev/null; then
    ensure_narrow_has_mode "$narrow" "$pos"
    echo "[elc-x11] xrandr: $wide @0x0 + $narrow @${pos}x0 (overlap / 1R-2-1L, auto) rotate=${ELC_X11_ROTATE}"
    overlap_warn_if_screen_narrow
    return 0
  fi
  return 1
}

run_side_by_side() {
  local a="$1"
  local b="$2"
  local wm nm narrow_out_rot
  wm=$(pick_wide_mode_name "$a")
  nm=$(pick_narrow_mode_name "$b")
  if [[ ${#NARROW_ROT[@]} -gt 0 ]]; then
    narrow_out_rot=("${NARROW_ROT[@]}")
  else
    narrow_out_rot=("${XR_ROT[@]}")
  fi
  xrandr --fb 2880x1080 2>/dev/null || true
  if [[ -n "$wm" && -n "$nm" ]]; then
    if xrandr --output "$a" --mode "$wm" "${XR_ROT[@]}" --pos 0x0 \
        --output "$b" --mode "$nm" "${narrow_out_rot[@]}" --right-of "$a" 2>/dev/null; then
      echo "[elc-x11] xrandr: $a mode=${wm} | $b mode=${nm} (side-by-side / explicit) wide_rot=${ELC_X11_ROTATE} narrow_rot=${ELC_X11_ROTATE_NARROW:-none}"
      return 0
    fi
  fi
  xrandr --output "$a" --auto "${XR_ROT[@]}" --pos 0x0 --output "$b" --auto "${narrow_out_rot[@]}" --right-of "$a"
  echo "[elc-x11] xrandr: $a | $b (side-by-side / auto) wide_rot=${ELC_X11_ROTATE} narrow_rot=${ELC_X11_ROTATE_NARROW:-none}"
}

if [[ -n "$LEFT" && -n "$RIGHT" ]]; then
  if [[ "$LAYOUT" == "side_by_side" ]]; then
    run_side_by_side "$LEFT" "$RIGHT"
    exit 0
  fi
  WA=$(get_width "$LEFT")
  WB=$(get_width "$RIGHT")
  if [[ "$WA" -ge "$WB" ]]; then
    WIDE="$LEFT"
    NARROW="$RIGHT"
  else
    WIDE="$RIGHT"
    NARROW="$LEFT"
  fi
  WW=$(get_width "$WIDE")
  NW=$(get_width "$NARROW")
  if [[ "$LAYOUT" == "overlap" ]] || { [[ "$LAYOUT" == "auto" ]] && [[ "$WW" -ge 2000 ]] && [[ "$NW" -le 800 ]]; }; then
    if run_overlap "$WIDE" "$NARROW"; then
      exit 0
    fi
  fi
  run_side_by_side "$LEFT" "$RIGHT"
  exit 0
fi

if [[ -n "$LEFT" || -n "$RIGHT" ]]; then
  echo "[elc-x11] set both ELC_X11_OUTPUT_LEFT and ELC_X11_OUTPUT_RIGHT, or neither" >&2
  exit 1
fi

A="${CONNECTED[0]}"
B="${CONNECTED[1]}"

# Both connectors can briefly report the same width (e.g. 2160×1080); prefer DRM mode lists (720×720 = 4″).
if [[ -n "${EL_INFERRED_WIDE:-}" && -n "${EL_INFERRED_NARROW:-}" ]]; then
  if [[ "$LAYOUT" == "side_by_side" ]]; then
    run_side_by_side "$EL_INFERRED_WIDE" "$EL_INFERRED_NARROW"
    exit 0
  fi
  if [[ "$LAYOUT" == "overlap" ]] || [[ "$LAYOUT" == "auto" ]]; then
    # Never swap wide/narrow here: run_overlap always puts $1 at 0x0 and $2 at +overlap. Reversing would put the 4″
    # on the left (pixels 0–720) so the round panel shows the speedo strip — the failure mode you saw during tuning.
    if run_overlap "$EL_INFERRED_WIDE" "$EL_INFERRED_NARROW"; then exit 0; fi
  fi
fi

WA=$(get_width "$A")
WB=$(get_width "$B")

if [[ "$LAYOUT" == "side_by_side" ]]; then
  run_side_by_side "$A" "$B"
  exit 0
fi

if [[ "$LAYOUT" == "overlap" ]]; then
  if [[ "$WA" -ge 2000 ]] && [[ "$WB" -le 800 ]]; then
    run_overlap "$A" "$B" && exit 0
  elif [[ "$WB" -ge 2000 ]] && [[ "$WA" -le 800 ]]; then
    run_overlap "$B" "$A" && exit 0
  fi
  # Do not swap A/B: same as inferred path — reversed overlap puts the small HDMI on the left (speedo on the 4″).
  if run_overlap "$A" "$B"; then exit 0; fi
  echo "[elc-x11] overlap failed; using side-by-side (set round_four_inch x=2160 in dashboard if 4″ stays black)" >&2
  run_side_by_side "$A" "$B"
  exit 0
fi

# auto: overlap when one output is clearly the 4in (≤800px wide) and the other is the splitter (≥2000px)
if [[ "$WA" -ge 2000 ]] && [[ "$WB" -le 800 ]]; then
  run_overlap "$A" "$B" && exit 0
fi
if [[ "$WB" -ge 2000 ]] && [[ "$WA" -le 800 ]]; then
  run_overlap "$B" "$A" && exit 0
fi
# Widths differ but heuristics missed (e.g. 1920 vs 720): try wider as first output.
if [[ "$WA" -gt 0 ]] && [[ "$WB" -gt 0 ]] && [[ "$WA" -ne "$WB" ]]; then
  if [[ "$WA" -gt "$WB" ]]; then
    if [[ "$WB" -le 800 ]]; then run_overlap "$A" "$B" && exit 0; fi
  else
    if [[ "$WA" -le 800 ]]; then run_overlap "$B" "$A" && exit 0; fi
  fi
fi

run_side_by_side "$A" "$B"
exit 0
