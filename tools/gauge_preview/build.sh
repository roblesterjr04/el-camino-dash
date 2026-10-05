#!/bin/bash
# Build the gauge preview renderer for the host (macOS/Linux, clang): LVGL 8.4 + the shared fonts + the
# firmware's gauge faces, the call card (phone_can) and the menu (ui_menu). LVGL comes from any PlatformIO build of the firmware, or set LVGL_DIR.
set -e
H=$(cd "$(dirname "$0")" && pwd)
FW=$(cd "$H/../../firmware" && pwd)
LV=${LVGL_DIR:-$FW/esp32_p4_display/.pio/libdeps/display-waveshare3c4/lvgl}
[ -f "$LV/lvgl.h" ] || { echo "LVGL not found at $LV: build the display firmware once, or set LVGL_DIR"; exit 1; }
O=$H/build
mkdir -p $O/lvgl $O/fonts
CF="-O2 -DLV_CONF_INCLUDE_SIMPLE -I$H/stub -I$FW/esp32_p4_ui/include -I$FW/elc_can/include -I$LV -I$LV/src -w"
for f in $(find $LV/src -name '*.c'); do
  o=$O/lvgl/$(echo ${f#$LV/src/} | tr / _).o
  [ -f $o ] || clang $CF -c $f -o $o &
done
for f in $FW/esp32_p4_ui/src/fonts/*.c; do
  o=$O/fonts/$(basename $f .c).o
  [ $o -nt $f ] || clang $CF -c $f -o $o &
done
wait
clang++ -std=c++17 $CF -I$FW/esp32_p4_ui/src/gauges $FW/esp32_p4_ui/src/gauges/*.cpp \
  $FW/esp32_p4_ui/src/phone_can.cpp $FW/esp32_p4_ui/src/ui_menu.cpp $FW/esp32_p4_ui/src/ui_theme.cpp $FW/elc_can/src/elc_can_codec.cpp \
  $FW/elc_can/src/elc_chan.cpp $FW/elc_can/src/elc_chan_msg.cpp $FW/elc_can/src/accessory_can.cpp $H/stubs.cpp $H/render.cpp \
  $O/lvgl/*.o $O/fonts/*.o -o $O/render
echo "built $O/render"
