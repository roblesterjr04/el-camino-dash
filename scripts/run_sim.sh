#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_VENV_DIR="${PROJECT_DIR}/.venv"
if [[ "${PROJECT_DIR}" == /mnt/shared* ]]; then
  DEFAULT_VENV_DIR="${HOME}/.el-camino-dash/.venv"
fi
VENV_DIR="${EL_CAMINO_VENV_DIR:-${DEFAULT_VENV_DIR}}"
PYTHON_BIN="${VENV_DIR}/bin/python"
CONFIG_PATH="${1:-${PROJECT_DIR}/config/dashboard.dev.yaml}"

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

echo "Starting dashboard simulation with config: ${CONFIG_PATH}"
exec env PYTHONPATH="${PROJECT_DIR}/src" "${PYTHON_BIN}" "${PROJECT_DIR}/src/main.py" --config "${CONFIG_PATH}"
