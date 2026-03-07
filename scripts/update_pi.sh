#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: bash scripts/update_pi.sh [options]

Syncs the current repo to the Pi runtime directory (default: ~/gauge-cluster)
without running the full installer.

Options:
  --project-dir <path>  Target runtime directory (default: ~/gauge-cluster)
  --with-pip            Also install/update Python requirements in .venv
  --help                Show this help
USAGE
}

PROJECT_DIR=""
WITH_PIP=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --project-dir=*)
      PROJECT_DIR="${1#*=}"
      shift
      ;;
    --project-dir)
      PROJECT_DIR="${2:-}"
      if [[ -z "${PROJECT_DIR}" ]]; then
        echo "Missing value for --project-dir" >&2
        exit 2
      fi
      shift 2
      ;;
    --with-pip)
      WITH_PIP=1
      shift
      ;;
    -h|--help|--usage)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! command -v rsync >/dev/null 2>&1; then
  echo "rsync is required but not found." >&2
  exit 1
fi

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_USER="${SUDO_USER:-${USER}}"
TARGET_HOME="$(getent passwd "${TARGET_USER}" | cut -d: -f6)"
if [[ -z "${TARGET_HOME}" || ! -d "${TARGET_HOME}" ]]; then
  echo "Unable to resolve home directory for user: ${TARGET_USER}" >&2
  exit 1
fi

if [[ -z "${PROJECT_DIR}" ]]; then
  PROJECT_DIR="${TARGET_HOME}/gauge-cluster"
fi

echo "Sync source: ${SOURCE_DIR}"
echo "Sync target: ${PROJECT_DIR}"

install -d "${PROJECT_DIR}"
rsync -a --delete \
  --exclude ".git/" \
  --exclude ".venv/" \
  --exclude "__pycache__/" \
  --exclude "*.pyc" \
  --exclude ".cursor/" \
  --exclude ".DS_Store" \
  "${SOURCE_DIR}/" "${PROJECT_DIR}/"

if [[ "${EUID}" -eq 0 ]]; then
  chown -R "${TARGET_USER}:${TARGET_USER}" "${PROJECT_DIR}"
fi

if [[ "${WITH_PIP}" -eq 1 ]]; then
  VENV_PY="${PROJECT_DIR}/.venv/bin/python"
  if [[ ! -x "${VENV_PY}" ]]; then
    echo "No virtualenv found at ${PROJECT_DIR}/.venv; skipping pip update."
  elif [[ ! -f "${PROJECT_DIR}/requirements.txt" ]]; then
    echo "No requirements.txt found; skipping pip update."
  else
    echo "Updating Python dependencies in ${PROJECT_DIR}/.venv..."
    if [[ "${EUID}" -eq 0 ]]; then
      sudo -u "${TARGET_USER}" "${VENV_PY}" -m pip install -r "${PROJECT_DIR}/requirements.txt"
    else
      "${VENV_PY}" -m pip install -r "${PROJECT_DIR}/requirements.txt"
    fi
  fi
fi

echo "Update sync complete."
