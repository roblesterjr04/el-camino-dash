#!/usr/bin/env bash
# Build and run host-side unit tests for the console UI state machine (no ESP32 toolchain needed).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
proj="$here/../.."
can="$proj/../elc_can"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-const-variable -O1 -fsanitize=address,undefined \
  -I "$proj/src" -I "$can/include" \
  "$can/src/elc_can_codec.cpp" "$can/src/elc_chan.cpp" "$can/src/elc_chan_msg.cpp" "$can/src/accessory_can.cpp" \
  "$proj/src/console_ui.cpp" "$proj/src/ir_remote.cpp" "$here/test_console_ui.cpp" \
  -o "$out/test_console_ui"
"$out/test_console_ui"
