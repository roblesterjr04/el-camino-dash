#!/usr/bin/env bash
set -euo pipefail

# Flash encoder_controller for Raspberry Pi Pico over USB serial.
# Legacy name kept so existing docs/scripts still work.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCR="${ROOT}/arduino/scripts"

if [[ $# -eq 0 ]]; then
  exec bash "${SCR}/upload_rp2040_sketch.sh" encoder_controller
fi
exec bash "${SCR}/upload_rp2040_sketch.sh" encoder_controller "$@"
