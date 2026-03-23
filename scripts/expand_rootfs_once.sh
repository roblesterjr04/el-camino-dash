#!/usr/bin/env bash
set -euo pipefail

STATE_DIR="/var/lib/el-camino-dash"
STATE_FILE="${STATE_DIR}/rootfs-expanded"
LOG_TAG="elc-expand-rootfs"

log() {
  local msg="$1"
  echo "[${LOG_TAG}] ${msg}"
  logger -t "${LOG_TAG}" "${msg}" >/dev/null 2>&1 || true
}

if [[ -f "${STATE_FILE}" ]]; then
  log "Root filesystem already expanded; skipping."
  exit 0
fi

ROOT_DEV="$(findmnt -n -o SOURCE / || true)"
FS_TYPE="$(findmnt -n -o FSTYPE / || true)"
if [[ -z "${ROOT_DEV}" || "${ROOT_DEV}" != /dev/* ]]; then
  log "Unsupported root device source '${ROOT_DEV}'; skipping."
  exit 0
fi

DISK_DEV=""
PART_NUM=""
case "${ROOT_DEV}" in
  /dev/mmcblk*p[0-9]*|/dev/nvme*n*p[0-9]*)
    DISK_DEV="${ROOT_DEV%p*}"
    PART_NUM="${ROOT_DEV##*p}"
    ;;
  /dev/sd*[0-9]*|/dev/vd*[0-9]*|/dev/xvd*[0-9]*)
    DISK_DEV="${ROOT_DEV%%[0-9]*}"
    PART_NUM="${ROOT_DEV##*[!0-9]}"
    ;;
  *)
    log "Unsupported root device '${ROOT_DEV}'; skipping."
    exit 0
    ;;
esac

if ! command -v growpart >/dev/null 2>&1; then
  log "growpart is not available; cannot expand root partition."
  exit 1
fi

log "Expanding partition ${DISK_DEV} partition ${PART_NUM}..."
set +e
GROWPART_OUTPUT="$(growpart "${DISK_DEV}" "${PART_NUM}" 2>&1)"
GROWPART_STATUS=$?
set -e
if [[ "${GROWPART_STATUS}" -ne 0 ]]; then
  if printf "%s\n" "${GROWPART_OUTPUT}" | rg -q "NOCHANGE"; then
    log "Partition already uses all available space."
  else
    printf "%s\n" "${GROWPART_OUTPUT}" >&2
    log "growpart failed for ${DISK_DEV} partition ${PART_NUM}."
    exit 1
  fi
else
  printf "%s\n" "${GROWPART_OUTPUT}"
fi

case "${FS_TYPE}" in
  ext2|ext3|ext4)
    if ! command -v resize2fs >/dev/null 2>&1; then
      log "resize2fs is unavailable; cannot expand ${FS_TYPE} filesystem."
      exit 1
    fi
    log "Resizing ${FS_TYPE} filesystem on ${ROOT_DEV}..."
    resize2fs "${ROOT_DEV}"
    ;;
  *)
    log "Filesystem type '${FS_TYPE}' is not managed by this script; skipping filesystem resize."
    ;;
esac

install -d -m 0755 "${STATE_DIR}"
touch "${STATE_FILE}"
log "Root filesystem expansion completed."
