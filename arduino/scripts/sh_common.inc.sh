# Shared helpers for arduino/*.sh. Caller must set:
#   ARDUINO_SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# before: source "${ARDUINO_SCRIPTS_DIR}/sh_common.inc.sh"

elc_detect_serial_port() {
  local kind="${1:-any}"
  python3 "${ARDUINO_SCRIPTS_DIR}/detect_serial_port.py" "${kind}"
}

elc_arduino_core_ensure() {
  local fqbn="$1"
  local required_core
  required_core="$(printf "%s" "${fqbn}" | awk -F: '{print $1 ":" $2}')"
  arduino-cli core update-index || echo "WARN: arduino-cli core update-index failed (offline? continuing.)" >&2
  if ! arduino-cli core list | awk -v core="${required_core}" 'NR > 1 && $1 == core {found=1} END {exit found ? 0 : 1}'; then
    echo "Installing Arduino core: ${required_core}" >&2
    arduino-cli core install "${required_core}"
  fi
}
