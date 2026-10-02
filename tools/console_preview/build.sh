#!/bin/bash
# Build the console OLED preview for the host (clang): the console's own UI, screens and fonts, and LVGL's font code
# (from a PlatformIO build of firmware/esp32s3_console, or set LVGL_DIR). Then: python3 preview.py
set -e
H=$(cd "$(dirname "$0")" && pwd)
FW=$(cd "$H/../../firmware" && pwd)
C=$FW/esp32s3_console
LV=${LVGL_DIR:-$C/.pio/libdeps/console/lvgl}
[ -f "$LV/lvgl.h" ] || { echo "LVGL not found at $LV: build the console firmware once, or set LVGL_DIR"; exit 1; }
O=$H/build
mkdir -p $O
CF="-O2 -DLV_CONF_INCLUDE_SIMPLE -I$C/include -I$C/src -I$FW/elc_can/include -I$LV -I$LV/src -w"
objs=""
for f in $LV/src/font/lv_font.c $LV/src/font/lv_font_fmt_txt.c $LV/src/misc/lv_utils.c $C/src/fonts/*.c; do
  o=$O/$(basename $f .c).o
  [ $o -nt $f ] || clang $CF -c $f -o $o
  objs="$objs $o"
done
clang++ -std=c++17 $CF $C/src/console_ui.cpp $C/src/console_screens.cpp $C/src/oled_gfx.cpp \
  $FW/elc_can/src/elc_can_codec.cpp $FW/elc_can/src/elc_chan.cpp $FW/elc_can/src/elc_chan_msg.cpp \
  $FW/elc_can/src/accessory_can.cpp $H/render.cpp $objs -o $O/render
echo "built $O/render"
