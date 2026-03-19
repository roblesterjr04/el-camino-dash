#!/usr/bin/env bash
set -u

LOG="/boot/elc-ssh-auth.log"
if [[ -d /boot/firmware ]]; then
  LOG="/boot/firmware/elc-ssh-auth.log"
fi
mkdir -p "$(dirname "$LOG")"
touch "$LOG"

run_cmd() {
  local cmd="$*"
  {
    echo "\$ ${cmd}"
    "$@" 2>&1
    local rc=$?
    if [[ $rc -ne 0 ]]; then
      echo "(exit_code=${rc})"
    fi
  } >>"$LOG"
}

{
  echo "===== elc ssh diagnostics start $(date -Is) ====="
  echo "host: $(hostname)"
} >>"$LOG"

run_cmd systemctl is-active ssh
run_cmd systemctl status --no-pager ssh
run_cmd bash -lc "sshd -T | grep -Ei 'pubkeyauthentication|passwordauthentication|kbdinteractiveauthentication|challengeresponseauthentication|authorizedkeysfile|permitrootlogin'"
run_cmd ls -la /home/pi/.ssh
run_cmd stat -c '%A %U:%G %n' /home/pi /home/pi/.ssh /home/pi/.ssh/authorized_keys
run_cmd bash -lc "ssh-keygen -lf /home/pi/.ssh/authorized_keys || true"
run_cmd journalctl -b --no-pager -u ssh -n 200

{
  echo "===== elc ssh diagnostics end $(date -Is) ====="
} >>"$LOG"
