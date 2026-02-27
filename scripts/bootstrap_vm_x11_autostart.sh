#!/usr/bin/env bash
set -euo pipefail

DRY_RUN=0
RUN_USER="${SUDO_USER:-${USER}}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_PATH="${PROJECT_DIR}/config/dashboard.vm.yaml"
SESSION="openbox"
RESET_TO_MULTI_USER=0

for arg in "$@"; do
  case "$arg" in
    --dry-run)
      DRY_RUN=1
      ;;
    --user=*)
      RUN_USER="${arg#*=}"
      ;;
    --project-dir=*)
      PROJECT_DIR="${arg#*=}"
      ;;
    --config=*)
      CONFIG_PATH="${arg#*=}"
      ;;
    --session=*)
      SESSION="${arg#*=}"
      ;;
    --reset-to-multi-user)
      RESET_TO_MULTI_USER=1
      ;;
    *)
      echo "Unknown argument: ${arg}" >&2
      echo "Usage: $0 [--dry-run] [--user=<user>] [--project-dir=<path>] [--config=<path>] [--session=openbox] [--reset-to-multi-user]" >&2
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

if [[ ! -d "${PROJECT_DIR}" ]]; then
  echo "Project directory not found: ${PROJECT_DIR}" >&2
  exit 1
fi

if [[ ! -f "${CONFIG_PATH}" ]]; then
  echo "Config file not found: ${CONFIG_PATH}" >&2
  exit 1
fi

USER_HOME="$(getent passwd "${RUN_USER}" | cut -d: -f6)"
if [[ -z "${USER_HOME}" || ! -d "${USER_HOME}" ]]; then
  echo "Could not resolve home directory for user: ${RUN_USER}" >&2
  exit 1
fi

RUN_HW="${PROJECT_DIR}/scripts/run_hw.sh"
if [[ ! -x "${RUN_HW}" ]]; then
  echo "Missing or non-executable runner: ${RUN_HW}" >&2
  exit 1
fi

LIGHTDM_CONF="/etc/lightdm/lightdm.conf.d/60-el-camino-autologin.conf"
OPENBOX_DIR="${USER_HOME}/.config/openbox"
OPENBOX_AUTOSTART="${OPENBOX_DIR}/autostart"
CACHE_DIR="${USER_HOME}/.cache/el-camino-dash"
BOOTSTRAP_LOG="${CACHE_DIR}/bootstrap.log"
APP_LOG="${CACHE_DIR}/autostart.log"
LAUNCHER_SCRIPT="${CACHE_DIR}/launch-dashboard.sh"

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "[dry-run] would prepare ${CACHE_DIR} and log files"
else
  mkdir -p "${CACHE_DIR}"
  touch "${BOOTSTRAP_LOG}" "${APP_LOG}"
  chown -R "${RUN_USER}:${RUN_USER}" "${CACHE_DIR}"
fi

log_msg() {
  local msg="$1"
  echo "${msg}"
  if [[ "${DRY_RUN}" -ne 1 ]]; then
    printf "%s %s\n" "$(date -u +"%Y-%m-%dT%H:%M:%SZ")" "${msg}" >> "${BOOTSTRAP_LOG}"
  fi
}

log_msg "Bootstrap settings: user=${RUN_USER}, home=${USER_HOME}, project=${PROJECT_DIR}, config=${CONFIG_PATH}, session=${SESSION}"

run_cmd "apt-get update"
run_cmd "apt-get install -y xorg lightdm openbox x11-xserver-utils"

LIGHTDM_CONTENT="[Seat:*]
autologin-user=${RUN_USER}
autologin-user-timeout=0
user-session=${SESSION}
"

LAUNCHER_CONTENT="#!/usr/bin/env bash
set -euo pipefail
export DISPLAY=\"\${DISPLAY:-:0}\"
export XAUTHORITY=\"\${XAUTHORITY:-${USER_HOME}/.Xauthority}\"
sleep 2

if pgrep -f \"${PROJECT_DIR}/src/main.py --config ${CONFIG_PATH}\" >/dev/null 2>&1; then
  exit 0
fi

cd \"${PROJECT_DIR}\"
exec \"${RUN_HW}\" \"${CONFIG_PATH}\" >> \"${APP_LOG}\" 2>&1
"

OPENBOX_AUTOSTART_CONTENT="#!/usr/bin/env bash
xset -dpms
xset s off
xset s noblank

\"${LAUNCHER_SCRIPT}\" &
"

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "[dry-run] would write ${LIGHTDM_CONF}"
else
  mkdir -p "$(dirname "${LIGHTDM_CONF}")"
  printf "%s\n" "${LIGHTDM_CONTENT}" > "${LIGHTDM_CONF}"
fi

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "[dry-run] would write ${LAUNCHER_SCRIPT}"
else
  printf "%s\n" "${LAUNCHER_CONTENT}" > "${LAUNCHER_SCRIPT}"
  chown "${RUN_USER}:${RUN_USER}" "${LAUNCHER_SCRIPT}"
  chmod +x "${LAUNCHER_SCRIPT}"
fi

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "[dry-run] would write ${OPENBOX_AUTOSTART}"
else
  mkdir -p "${OPENBOX_DIR}"
  printf "%s\n" "${OPENBOX_AUTOSTART_CONTENT}" > "${OPENBOX_AUTOSTART}"
  chown -R "${RUN_USER}:${RUN_USER}" "${OPENBOX_DIR}"
  chmod +x "${OPENBOX_AUTOSTART}"
fi

run_cmd "systemctl enable lightdm"
if [[ "${RESET_TO_MULTI_USER}" -eq 1 ]]; then
  run_cmd "systemctl set-default multi-user.target"
else
  run_cmd "systemctl set-default graphical.target"
fi

log_msg "Bootstrap complete."
log_msg "User: ${RUN_USER}"
log_msg "Project: ${PROJECT_DIR}"
log_msg "Config: ${CONFIG_PATH}"
log_msg "Session: ${SESSION}"
log_msg "App launcher: ${LAUNCHER_SCRIPT}"
log_msg "Bootstrap log: ${BOOTSTRAP_LOG}"
log_msg "App log: ${APP_LOG}"
if [[ "${RESET_TO_MULTI_USER}" -eq 1 ]]; then
  log_msg "Default target set to multi-user.target"
else
  log_msg "Default target set to graphical.target"
fi
log_msg "Reboot to verify autologin + dashboard launch."
