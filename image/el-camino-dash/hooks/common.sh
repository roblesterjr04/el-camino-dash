#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="${ELC_SOURCE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
ROOTFS_DIR="${ELC_ROOTFS_DIR:-${ROOTFS_DIR:-}}"
TARGET_USER="${ELC_TARGET_USER:-pi}"
RUNTIME_DIR="${ELC_RUNTIME_DIR:-/opt/el-camino-dash}"

require_rootfs() {
  if [[ -z "${ROOTFS_DIR}" ]]; then
    echo "ELC_ROOTFS_DIR (or ROOTFS_DIR) must be set for image hook execution." >&2
    exit 1
  fi
  mkdir -p "${ROOTFS_DIR}"
}

in_rootfs() {
  local path="$1"
  printf "%s%s" "${ROOTFS_DIR}" "${path}"
}
