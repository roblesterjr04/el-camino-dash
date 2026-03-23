#!/usr/bin/env bash
set -euo pipefail

# Flash encoder_controller over ICSP (USB ISP). No args = usbtinyisp + port "usb" + Uno FQBN.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCR="${ROOT}/arduino/scripts"

if [[ $# -eq 0 ]]; then
  exec bash "${SCR}/upload_avr_sketch.sh" encoder_controller usb arduino:avr:uno --programmer usbtinyisp
fi
exec bash "${SCR}/upload_avr_sketch.sh" encoder_controller "$@"
