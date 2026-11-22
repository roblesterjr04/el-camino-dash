#!/usr/bin/env bash
# Host-side tests for the AI card's portable logic (no ESP32 toolchain needed): link framing, audio, policy, power
# sequencing, the session state machine, the menu page, the offline commands.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../../src"
lib="$here/../../../elc_can"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-const-variable -Wno-unused-private-field -O1 -fsanitize=address,undefined \
  -I "$src" -I "$lib/include" \
  "$src"/ai_link.cpp "$src"/ai_audio.cpp "$src"/ai_policy.cpp "$src"/ai_power.cpp "$src"/ai_session.cpp "$src"/ai_ui.cpp \
  "$src"/ai_commands.cpp "$lib/src/elc_chan.cpp" "$lib/src/elc_chan_msg.cpp" "$lib/src/elc_can_codec.cpp" \
  "$here/test_ai.cpp" -o "$out/test_ai"
"$out/test_ai"
