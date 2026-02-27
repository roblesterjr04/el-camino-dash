#!/usr/bin/env bash
set -euo pipefail

DRY_RUN=0
RUN_USER="${SUDO_USER:-${USER}}"
SET_MULTI_USER=0

for arg in "$@"; do
  case "$arg" in
    --dry-run)
      DRY_RUN=1
      ;;
    --user=*)
      RUN_USER="${arg#*=}"
      ;;
    --set-multi-user)
      SET_MULTI_USER=1
      ;;
    *)
      echo "Unknown argument: ${arg}" >&2
      echo "Usage: $0 [--dry-run] [--user=<user>] [--set-multi-user]" >&2
      exit 1
      ;;
  esac
done

run_cmd() {
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "[dry-run] $*"
  else
    eval "$@"
  fi
}

if [[ "${EUID}" -ne 0 ]]; then
  echo "This script must run as root (use sudo)." >&2
  exit 1
fi

USER_HOME="$(getent passwd "${RUN_USER}" | cut -d: -f6)"
if [[ -z "${USER_HOME}" || ! -d "${USER_HOME}" ]]; then
  echo "Could not resolve home directory for user: ${RUN_USER}" >&2
  exit 1
fi

LIGHTDM_CONF="/etc/lightdm/lightdm.conf.d/60-el-camino-autologin.conf"
OPENBOX_AUTOSTART="${USER_HOME}/.config/openbox/autostart"
CACHE_DIR="${USER_HOME}/.cache/el-camino-dash"
LAUNCHER_SCRIPT="${CACHE_DIR}/launch-dashboard.sh"

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "[dry-run] would remove ${LIGHTDM_CONF}"
  echo "[dry-run] would remove ${OPENBOX_AUTOSTART}"
  echo "[dry-run] would remove ${LAUNCHER_SCRIPT}"
else
  rm -f "${LIGHTDM_CONF}" "${OPENBOX_AUTOSTART}" "${LAUNCHER_SCRIPT}"
fi

if [[ "${SET_MULTI_USER}" -eq 1 ]]; then
  run_cmd "systemctl set-default multi-user.target"
fi

echo "Revert complete."
echo "Removed: ${LIGHTDM_CONF}"
echo "Removed: ${OPENBOX_AUTOSTART}"
echo "Removed: ${LAUNCHER_SCRIPT}"
if [[ "${SET_MULTI_USER}" -eq 1 ]]; then
  echo "Default target set to multi-user.target"
fi
echo "Reboot to apply."
