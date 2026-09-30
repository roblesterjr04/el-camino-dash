#!/usr/bin/env bash
# Build and run host-side unit tests for the CAN frame codecs (no ESP32 toolchain needed).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
lib="$here/../.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

c++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-const-variable -Wno-unused-private-field -O1 -fsanitize=address,undefined \
  -I "$lib/include" \
  "$lib/src/elc_can_codec.cpp" "$lib/src/elc_can.cpp" "$here/test_elc_can_codec.cpp" \
  -o "$out/test_elc_can_codec"
"$out/test_elc_can_codec"
