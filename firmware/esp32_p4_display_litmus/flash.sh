#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

VENV="${VENV:-.venv}"
PY="${VENV}/bin/python"
PIP="${VENV}/bin/pip"

PIO_TARGET="upload"
EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --erase-first|--factory)
      PIO_TARGET="erase_upload"
      shift
      ;;
    -h|--help)
      cat <<'EOF'
Usage: ./flash.sh [OPTION]... [--] [platformio args...]

  --erase-first, --factory   Full chip erase, then upload (PlatformIO erase_upload).
                            Use after changing flash/partition layout or if flash looks corrupt.
  -h, --help                This message.

Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C: use -e litmus-waveshare-3c4. The board has two Type-C
ports that both mention flashing — use the one that enumerates as the ESP32-P4 UART bridge (only
one cable plugged in while testing). Then:

  ./flash.sh --erase-first -e litmus-waveshare-3c4 --upload-port /dev/cu.usbmodem…

Serial at 115200 (confirm firmware is running — not just a blank LCD):

  .venv/bin/python -m platformio device monitor -e litmus-waveshare-3c4 --port /dev/cu.…

If USB-CDC logs are empty, try -e litmus-waveshare-3c4-uart or -e litmus-serial-only.
EOF
      exit 0
      ;;
    --)
      shift
      EXTRA_ARGS+=("$@")
      break
      ;;
    *)
      EXTRA_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ ! -x "${PY}" ]]; then
  python3 -m venv "${VENV}"
fi
if [[ -f requirements.txt ]]; then
  "${PIP}" install -q -r requirements.txt
fi

PIO_CORE="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
_have_gxx=""
for _root in "${PIO_CORE}/packages" "${PIO_CORE}/tools"; do
  [[ -d "${_root}" ]] || continue
  while IFS= read -r -d '' _d; do
    if [[ -x "${_d}/bin/riscv32-esp-elf-g++" ]]; then
      _have_gxx=1
      break 2
    fi
  done < <(find "${_root}" -maxdepth 1 \( -type d -name 'toolchain-riscv32-esp@*' -o -type d -name 'toolchain-riscv32-esp' \) -print0 2>/dev/null)
done
if [[ -z "${_have_gxx}" ]]; then
  for _root in "${PIO_CORE}/packages" "${PIO_CORE}/tools"; do
    find "${_root}" -maxdepth 1 \( -type d -name 'toolchain-riscv32-esp@*' -o -type d -name 'toolchain-riscv32-esp' \) -exec rm -rf {} + 2>/dev/null || true
  done
fi

run_pio() {
  "${PY}" -m platformio run -t "${PIO_TARGET}" "${EXTRA_ARGS[@]}"
}

set +e
run_pio
code=$?
set -e

if [[ -f .pio/toolchain_reinstall_retry ]]; then
  rm -f .pio/toolchain_reinstall_retry
  echo ""
  echo "Retrying once after toolchain cleanup (re-download)..."
  exec "${PY}" -m platformio run -t "${PIO_TARGET}" "${EXTRA_ARGS[@]}"
fi

exit "$code"
