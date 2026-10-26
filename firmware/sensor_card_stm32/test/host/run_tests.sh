#!/usr/bin/env bash
# Host-side tests for the rev B card (no ARM toolchain needed). The logic it shares with rev A (calibration, odometer
# + FRAM blocks, engine, channels, menu, frames) is tested by rev A's suite, run here as is; then this card's own
# pure parts: the ADC count scaling, the level inputs' thresholds, and the input-capture timestamp extension.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
"$here/../../../sensor_card/test/host/run_tests.sh"

out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I "$here/../../src")
c++ "${flags[@]}" "$here/test_hw_logic.cpp" -o "$out/test_hw_logic"
"$out/test_hw_logic"
