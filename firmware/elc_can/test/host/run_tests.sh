#!/usr/bin/env bash
# Build and run host-side unit tests for the CAN frame codecs (no ESP32 toolchain needed).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
lib="$here/../.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-const-variable -Wno-unused-private-field -O1 -fsanitize=address,undefined \
  -I "$lib/include" \
  "$lib/src/elc_can_codec.cpp" "$lib/src/elc_can.cpp" "$lib/src/elc_chan.cpp" "$lib/src/elc_usb_link.cpp" "$here/test_elc_can_codec.cpp" \
  -o "$out/test_elc_can_codec"
"$out/test_elc_can_codec"

c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-const-variable -O1 -fsanitize=address,undefined \
  -I "$lib/include" \
  "$lib/src/elc_chan.cpp" "$lib/src/elc_chan_msg.cpp" "$lib/src/elc_can_codec.cpp" "$here/test_elc_chan.cpp" \
  -o "$out/test_elc_chan"
"$out/test_elc_chan"
