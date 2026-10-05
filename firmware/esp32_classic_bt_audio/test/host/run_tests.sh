#!/usr/bin/env bash
# Build and run the host tests for the audio bridge's chimes (no ESP32 toolchain needed).
# ./run_tests.sh --wav <dir> also writes each chime as a 44.1 kHz WAV, to listen to.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
sketch="$here/../../../../arduino/sketches/esp32_audio_bridge"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

c++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
  -I "$sketch" "$sketch/chimes.cpp" "$here/test_chimes.cpp" -o "$out/test_chimes"
"$out/test_chimes" "$@"
