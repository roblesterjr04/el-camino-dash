#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="${PROJECT_DIR}/.venv"
PYTHON_BIN="${VENV_DIR}/bin/python"
PIP_BIN="${VENV_DIR}/bin/pip"
CONFIG_PATH="${1:-${PROJECT_DIR}/config/dashboard.dev.yaml}"

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
if [[ -x "${PYTHON_BIN}" ]]; then
  current_minor="$("${PYTHON_BIN}" - <<'PY'
import sys
print(sys.version_info.minor)
PY
)"
  if [[ "${current_minor}" -ge 14 ]]; then
    venv_needs_rebuild=1
  fi
fi

if [[ ! -d "${VENV_DIR}" || "${venv_needs_rebuild}" -eq 1 ]]; then
  if [[ "${venv_needs_rebuild}" -eq 1 ]]; then
    echo "Existing virtualenv uses unsupported Python 3.${current_minor}; rebuilding with ${SELECTED_PYTHON}"
    rm -rf "${VENV_DIR}"
  fi
  echo "Creating virtual environment at ${VENV_DIR} using ${SELECTED_PYTHON}"
  "${SELECTED_PYTHON}" -m venv "${VENV_DIR}"
fi

echo "Installing/updating dependencies"
"${PIP_BIN}" install --upgrade pip
"${PIP_BIN}" install -r "${PROJECT_DIR}/requirements.txt"

echo "Starting dashboard simulation with config: ${CONFIG_PATH}"
exec env PYTHONPATH="${PROJECT_DIR}/src" "${PYTHON_BIN}" "${PROJECT_DIR}/src/main.py" --config "${CONFIG_PATH}"
