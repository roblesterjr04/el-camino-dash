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

  --erase-first, --factory   Full chip erase then upload (erase_upload).
  -h, --help                This message.

Example (Waveshare 3.4C):
  ./flash.sh --erase-first -e display-waveshare3c4 --upload-port /dev/cu.usbmodem…
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

# Homebrew Python is PEP 668 "externally managed" — install PlatformIO inside a local venv.
if [[ ! -x "${PY}" ]]; then
  python3 -m venv "${VENV}"
fi
if [[ -f requirements.txt ]]; then
  "${PIP}" install -q -r requirements.txt
fi

# Compiler may live under ~/.platformio/tools/toolchain-riscv32-esp* (pioarduino), not only packages/.
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
  echo "RISC-V toolchain missing under ${PIO_CORE}/{packages,tools}; removing cached toolchain dirs for a clean reinstall."
  for _root in "${PIO_CORE}/packages" "${PIO_CORE}/tools"; do
    find "${_root}" -maxdepth 1 \( -type d -name 'toolchain-riscv32-esp@*' -o -type d -name 'toolchain-riscv32-esp' \) -exec rm -rf {} + 2>/dev/null || true
  done
fi

run_pio() {
  # file://../esp32_p4_common can stay stuck on an old copy in libdeps; always reinstall from the repo.
  rm -rf .pio/libdeps/*/el_camino_p4_common 2>/dev/null || true
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
