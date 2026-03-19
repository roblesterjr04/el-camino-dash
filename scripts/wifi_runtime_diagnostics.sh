#!/usr/bin/env bash
set -u

LOG="/boot/elc-wifi-runtime-diag.log"
if [[ -d /boot/firmware ]]; then
  LOG="/boot/firmware/elc-wifi-runtime-diag.log"
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

capture_snapshot() {
  local label="$1"
  {
    echo
    echo "===== ${label} $(date -Is) ====="
  } >>"$LOG"

  run_cmd systemctl is-active NetworkManager.service
  run_cmd systemctl is-active elc-wifi-radio-on.service
  run_cmd systemctl is-active systemd-networkd.service
  run_cmd systemctl is-active wpa_supplicant.service
  run_cmd nmcli -f DEVICE,TYPE,STATE,CONNECTION device
  run_cmd nmcli -f NAME,UUID,TYPE,AUTOCONNECT,DEVICE connection show
  run_cmd rfkill list
  run_cmd ip -brief link
  if command -v iw >/dev/null 2>&1; then
    run_cmd iw dev
  else
    {
      echo "\$ iw dev"
      echo "iw not installed"
    } >>"$LOG"
  fi
}

{
  echo "===== elc wifi runtime diagnostics start $(date -Is) ====="
  echo "host: $(hostname)"
  echo "kernel: $(uname -r)"
} >>"$LOG"

run_cmd uname -a
run_cmd systemctl is-enabled NetworkManager.service
run_cmd systemctl is-enabled elc-wifi-radio-on.service
run_cmd systemctl is-enabled systemd-networkd.service
run_cmd systemctl is-enabled wpa_supplicant.service
run_cmd systemctl status --no-pager NetworkManager.service
run_cmd systemctl status --no-pager elc-wifi-radio-on.service
run_cmd systemctl status --no-pager systemd-networkd.service
run_cmd systemctl status --no-pager wpa_supplicant.service
run_cmd nmcli general status
run_cmd nmcli radio

capture_snapshot "snapshot-0"
sleep 15
capture_snapshot "snapshot-15s"
sleep 30
capture_snapshot "snapshot-45s"

run_cmd lsmod
run_cmd dmesg -T
run_cmd journalctl -b --no-pager -u NetworkManager.service -n 200
run_cmd journalctl -b --no-pager -u elc-wifi-radio-on.service -n 200
run_cmd journalctl -b --no-pager -u systemd-networkd.service -n 120
run_cmd journalctl -b --no-pager -u wpa_supplicant.service -n 120

if [[ -f /boot/firmware/config.txt ]]; then
  run_cmd sed -n '1,220p' /boot/firmware/config.txt
elif [[ -f /boot/config.txt ]]; then
  run_cmd sed -n '1,220p' /boot/config.txt
fi

{
  echo
  echo "===== filtered hints $(date -Is) ====="
} >>"$LOG"
run_cmd bash -lc "dmesg -T | grep -Ei 'brcm|brcmfmac|cfg80211|wlan0|firmware|rfkill|regulatory'"
run_cmd bash -lc "journalctl -b --no-pager | grep -Ei 'NetworkManager|wpa_supplicant|wlan0|brcm|brcmfmac|rfkill|firmware'"

{
  echo
  echo "===== elc wifi runtime diagnostics end $(date -Is) ====="
} >>"$LOG"
