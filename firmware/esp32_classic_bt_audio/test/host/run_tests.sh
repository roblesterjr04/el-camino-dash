#!/usr/bin/env bash
# Build and run the host tests for the audio card's chimes and signal processing (no ESP32 toolchain needed).
# ./run_tests.sh --wav <dir> also writes each chime as a 44.1 kHz WAV, to listen to.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
sketch="$here/../../../../arduino/sketches/esp32_audio_bridge"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

flags=(-std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I "$sketch")
c++ "${flags[@]}" "$sketch/chimes.cpp" "$sketch/audio_dsp.cpp" "$here/test_chimes.cpp" -o "$out/test_chimes"
"$out/test_chimes" "$@"
c++ "${flags[@]}" "$sketch/audio_dsp.cpp" "$here/test_audio_dsp.cpp" -o "$out/test_audio_dsp"
"$out/test_audio_dsp"
can="$here/../../../elc_can"
c++ "${flags[@]}" -Wno-unused-const-variable -I "$can/include" \
  "$can/src/elc_can_codec.cpp" "$can/src/elc_chan.cpp" "$can/src/elc_chan_msg.cpp" \
  "$sketch/audio_dsp.cpp" "$sketch/sink.cpp" "$here/test_sink.cpp" -o "$out/test_sink"
"$out/test_sink"
