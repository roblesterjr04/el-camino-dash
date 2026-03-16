from __future__ import annotations

import subprocess
import time
from typing import Any


class BluetoothMetadataAdapter:
    def __init__(self, config: dict[str, Any]) -> None:
        self._cfg = config
        self._status = "disabled"
        self._last_refresh_s = 0.0
        self._player_state = ""
        self._track_title = ""
        self._track_artist = ""

    def status(self) -> str:
        if not self._cfg.get("enabled", False):
            return "disabled"
        return self._status

    def start(self) -> None:
        if not self._cfg.get("enabled", False):
            self._status = "disabled"
            return
        self._refresh_status()

    def poll(self) -> None:
        if not self._cfg.get("enabled", False):
            self._status = "disabled"
            return
        interval_s = max(1.0, float(self._cfg.get("refresh_interval_s", 2.0)))
        now = time.monotonic()
        if (now - self._last_refresh_s) < interval_s:
            return
        self._last_refresh_s = now
        self._refresh_status()

    def _refresh_status(self) -> None:
        provider = str(self._cfg.get("provider", "mock")).strip().lower()
        if provider not in {"bluez", "bluetoothctl"}:
            self._status = f"enabled:{provider}"
            return

        device_mac = self._resolved_device_mac()
        if not device_mac:
            self._status = "enabled:bluez:no-device"
            self._player_state = ""
            self._track_title = ""
            self._track_artist = ""
            return
        if not self._is_connected(device_mac):
            self._status = "enabled:bluez:disconnected"
            self._player_state = ""
            self._track_title = ""
            self._track_artist = ""
            return

        self._refresh_player_metadata()
        if self._player_state:
            self._status = f"enabled:bluez:{self._player_state}"
            return
        self._status = "enabled:bluez:connected"

    def _refresh_player_metadata(self) -> None:
        self._player_state = ""
        self._track_title = ""
        self._track_artist = ""
        result = self._run(["bluetoothctl", "player.show"], allow_fail=True, timeout_s=5.0)
        if result.returncode != 0:
            return
        for raw_line in result.stdout.splitlines():
            line = raw_line.strip()
            if line.startswith("Status:"):
                self._player_state = line.split(":", 1)[1].strip().lower()
            elif line.startswith("Title:"):
                self._track_title = line.split(":", 1)[1].strip()
            elif line.startswith("Artist:"):
                self._track_artist = line.split(":", 1)[1].strip()

    def _device_mac(self) -> str:
        raw = str(self._cfg.get("device_mac", "")).strip()
        if not raw:
            return ""
        return raw.upper()

    def _resolved_device_mac(self) -> str:
        configured = self._device_mac()
        if configured:
            return configured
        connected = self._connected_device_macs()
        if connected:
            return connected[0]
        return ""

    def _connected_device_macs(self) -> list[str]:
        result = self._run(["bluetoothctl", "devices", "Connected"], allow_fail=True, timeout_s=5.0)
        if result.returncode != 0:
            return []
        macs: list[str] = []
        for line in result.stdout.splitlines():
            line = line.strip()
            if not line.startswith("Device "):
                continue
            parts = line.split()
            if len(parts) >= 2 and ":" in parts[1]:
                macs.append(parts[1].upper())
        return macs

    def _is_connected(self, device_mac: str) -> bool:
        result = self._run(["bluetoothctl", "info", device_mac], allow_fail=True, timeout_s=5.0)
        if result.returncode != 0:
            return False
        for line in result.stdout.splitlines():
            if "Connected:" in line:
                return line.strip().lower().endswith("yes")
        return False

    def _run(
        self,
        cmd: list[str],
        allow_fail: bool = False,
        timeout_s: float = 10.0,
    ) -> subprocess.CompletedProcess[str]:
        effective_timeout = max(0.2, float(self._cfg.get("command_timeout_s", timeout_s)))
        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=effective_timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            proc = subprocess.CompletedProcess(cmd, 124, "", "timeout")
        if (not allow_fail) and proc.returncode != 0:
            raise RuntimeError(f"command failed: {' '.join(cmd)}: {proc.stderr.strip()}")
        return proc
