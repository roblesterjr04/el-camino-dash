#!/usr/bin/env bash
# Host-side tests for the sensor card's logic (no AVR toolchain needed): calibration, odometer, engine, channels.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../../src"
lib="$here/../../../elc_can"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -Wno-unused-const-variable -Wno-unused-private-field -O1 -fsanitize=address,undefined
       -I "$src" -I "$lib/include")
c++ "${flags[@]}" "$src/calib.cpp" "$src/odometer.cpp" "$src/engine.cpp" "$lib/src/elc_chan.cpp" "$lib/src/elc_can_codec.cpp" \
  "$here/test_logic.cpp" -o "$out/test_logic"

"$out/test_logic"

c++ "${flags[@]}" "$src/calib.cpp" "$src/odometer.cpp" "$src/engine.cpp" "$src/card_net.cpp" "$src/card_ui.cpp" \
  "$src/card_chan.cpp" "$src/legacy.cpp" "$lib/src/elc_chan.cpp" "$lib/src/elc_chan_msg.cpp" "$lib/src/elc_can_codec.cpp" \
  "$here/test_net.cpp" -o "$out/test_net"
"$out/test_net"
