#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_VENV_DIR="${PROJECT_DIR}/.venv"
if [[ "${PROJECT_DIR}" == /mnt/shared* ]]; then
  DEFAULT_VENV_DIR="${HOME}/.el-camino-dash/.venv"
fi
VENV_DIR="${EL_CAMINO_VENV_DIR:-${DEFAULT_VENV_DIR}}"
PYTHON_BIN="${VENV_DIR}/bin/python"
CONFIG_ARG="dev.yaml"

usage() {
  echo "Usage: $0 [-c|--config <config-file>]"
  echo "  config-file can be:"
  echo "    - an absolute or relative path"
  echo "    - a file in config/ (for example: dashboard.dev.yaml)"
  echo "    - a short name resolved as config/dashboard.<name> (for example: dev.yaml)"
  echo ""
  echo "Environment (optional):"
  echo "  EL_CAMINO_OLED_SIM=0     Disable the 256×64 OLED UART simulator subprocess."
  echo "  EL_CAMINO_OLED_SIM_SCALE  Pixel scale for the OLED window (default 4)."
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -c|--config)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for $1" >&2
        usage
        exit 1
      fi
      CONFIG_ARG="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      # Backward-compatible positional config argument.
      CONFIG_ARG="$1"
      shift
      ;;
  esac
done

if [[ -f "${CONFIG_ARG}" ]]; then
  CONFIG_PATH="${CONFIG_ARG}"
elif [[ -f "${PROJECT_DIR}/config/${CONFIG_ARG}" ]]; then
  CONFIG_PATH="${PROJECT_DIR}/config/${CONFIG_ARG}"
elif [[ -f "${PROJECT_DIR}/config/dashboard.${CONFIG_ARG}" ]]; then
  CONFIG_PATH="${PROJECT_DIR}/config/dashboard.${CONFIG_ARG}"
else
  echo "Config file not found for argument: ${CONFIG_ARG}" >&2
  usage
  exit 1
fi

cd "${PROJECT_DIR}"

pick_python() {
  for candidate in python3.11 python3.10 python3; do
    if command -v "${candidate}" >/dev/null 2>&1; then
      local ver
      ver="$("${candidate}" - <<'PY'
import sys
print(f"{sys.version_info.major}.{sys.version_info.minor}")
PY
)"
      local major minor
      major="${ver%%.*}"
      minor="${ver##*.}"
      if [[ "${major}" -eq 3 && "${minor}" -lt 14 ]]; then
        echo "${candidate}"
        return 0
      fi
    fi
  done
  return 1
}

SELECTED_PYTHON="$(pick_python || true)"
if [[ -z "${SELECTED_PYTHON}" ]]; then
  echo "No supported python found (need Python 3.10-3.13 for pygame wheels)." >&2
  exit 1
fi

venv_needs_rebuild=0
if [[ -d "${VENV_DIR}" ]]; then
  if ! "${PYTHON_BIN}" -c "import sys" >/dev/null 2>&1; then
    echo "Existing virtualenv is not runnable on this machine; rebuilding."
    venv_needs_rebuild=1
  else
    current_minor="$("${PYTHON_BIN}" - <<'PY'
import sys
print(sys.version_info.minor)
PY
)"
    if [[ "${current_minor}" -ge 14 ]]; then
      venv_needs_rebuild=1
    fi
  fi
fi

if [[ ! -d "${VENV_DIR}" || "${venv_needs_rebuild}" -eq 1 ]]; then
  if [[ "${venv_needs_rebuild}" -eq 1 ]]; then
    echo "Rebuilding virtual environment with ${SELECTED_PYTHON}"
    rm -rf "${VENV_DIR}"
  fi
  echo "Creating virtual environment at ${VENV_DIR} using ${SELECTED_PYTHON}"
  mkdir -p "$(dirname "${VENV_DIR}")"
  "${SELECTED_PYTHON}" -m venv --copies "${VENV_DIR}"
fi

echo "Installing/updating dependencies"
"${PYTHON_BIN}" -m pip install --upgrade pip
"${PYTHON_BIN}" -m pip install -r "${PROJECT_DIR}/requirements.txt"

# Optional 256×64 OLED UART simulator (tools/oled_sim). Disabled when EL_CAMINO_OLED_SIM=0.
# When socat(1) is available, creates a linked PTY pair: dashboard writes on EL_CAMINO_NANO_BRIDGE_PORT,
# oled_sim reads the other end (same bytes as a real Nano would see on RX).
OLED_SIM_PID=""
SOCAT_UART_PID=""
UART_TMP=""
cleanup_oled_sim() {
  if [[ -n "${OLED_SIM_PID}" ]] && kill -0 "${OLED_SIM_PID}" 2>/dev/null; then
    kill "${OLED_SIM_PID}" 2>/dev/null || true
    wait "${OLED_SIM_PID}" 2>/dev/null || true
  fi
  if [[ -n "${SOCAT_UART_PID}" ]] && kill -0 "${SOCAT_UART_PID}" 2>/dev/null; then
    kill "${SOCAT_UART_PID}" 2>/dev/null || true
    wait "${SOCAT_UART_PID}" 2>/dev/null || true
  fi
  if [[ -n "${UART_TMP}" && -d "${UART_TMP}" ]]; then
    rm -rf "${UART_TMP}"
  fi
}

if [[ "${EL_CAMINO_OLED_SIM:-1}" != "0" && "${EL_CAMINO_OLED_SIM:-1}" != "false" ]]; then
  if command -v socat >/dev/null 2>&1; then
    UART_TMP="$(mktemp -d /tmp/elc-sim-uart.XXXXXX)"
    PI_LINK="${UART_TMP}/pi"
    OLED_LINK="${UART_TMP}/oled"
    socat -d -d \
      pty,raw,echo=0,link="${PI_LINK}" \
      pty,raw,echo=0,link="${OLED_LINK}" >/dev/null 2>&1 &
    SOCAT_UART_PID=$!
    for _ in $(seq 1 50); do
      if [[ -e "${PI_LINK}" && -e "${OLED_LINK}" ]]; then
        break
      fi
      sleep 0.05
    done
    if [[ -e "${PI_LINK}" && -e "${OLED_LINK}" ]]; then
      export EL_CAMINO_NANO_BRIDGE_PORT="${PI_LINK}"
      OLED_SCALE="${EL_CAMINO_OLED_SIM_SCALE:-4}"
      echo "Starting OLED UART simulator on ${OLED_LINK} (dashboard -> ${PI_LINK})"
      env PYTHONPATH="${PROJECT_DIR}/src" "${PYTHON_BIN}" "${PROJECT_DIR}/tools/oled_sim/oled_sim.py" \
        --port "${OLED_LINK}" --scale "${OLED_SCALE}" &
      OLED_SIM_PID=$!
    else
      echo "socat did not create PTY links in time; skipping OLED simulator." >&2
      cleanup_oled_sim
      UART_TMP=""
      SOCAT_UART_PID=""
    fi
  else
    echo "socat not found; skipping OLED simulator (install socat or set EL_CAMINO_OLED_SIM=0)." >&2
  fi
fi

echo "Starting dashboard simulation with config: ${CONFIG_PATH}"
set +e
env PYTHONPATH="${PROJECT_DIR}/src" "${PYTHON_BIN}" "${PROJECT_DIR}/src/main.py" --config "${CONFIG_PATH}"
MAIN_EXIT=$?
set -e
cleanup_oled_sim
exit "${MAIN_EXIT}"
