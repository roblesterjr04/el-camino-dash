#!/usr/bin/env bash
# Build and run host-side unit tests for the shared gauge code (no ESP32 toolchain needed).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
common="$here/../.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

c++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
  -I "$here" -I "$common/include" \
  "$common/src/sensor_packet.cpp" "$here/test_sensor_packet.cpp" \
  -o "$out/test_sensor_packet"
"$out/test_sensor_packet"
