#!/usr/bin/env bash
set -euo pipefail

# Flash power_control ATtiny over ICSP using a USB-connected ISP (default: usbtinyisp).

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec bash "${ROOT}/arduino/scripts/program_power_control_attiny.sh" --programmer usbtinyisp "$@"
