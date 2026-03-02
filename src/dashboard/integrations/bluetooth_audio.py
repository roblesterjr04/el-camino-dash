from __future__ import annotations

import subprocess
import time
from typing import Any


class BluetoothAudioAdapter:
    def __init__(self, config: dict[str, Any]) -> None:
        self._cfg = config
        self._status = "disabled"
        self._last_refresh_s = 0.0
        self._cached_sink = ""

    def status(self) -> str:
        if not self._cfg.get("enabled", False):
            return "disabled"
        return self._status

    def start(self) -> None:
        if not self._cfg.get("enabled", False):
            self._status = "disabled"
            return
        if bool(self._cfg.get("auto_connect", True)):
            self.reconnect()
            return
        self._refresh_status()

    def poll(self) -> None:
        if not self._cfg.get("enabled", False):
            self._status = "disabled"
            return
        interval_s = max(1.0, float(self._cfg.get("refresh_interval_s", 5.0)))
        now = time.monotonic()
        if (now - self._last_refresh_s) < interval_s:
            return
        self._last_refresh_s = now
        self._refresh_status()

    def reconnect(self) -> None:
        if not self._cfg.get("enabled", False):
            self._status = "disabled"
            return
        device_mac = self._device_mac()
        if not device_mac:
            self._status = "enabled:no-device"
            return
        try:
            self._run_bt(["power", "on"])
            self._run_bt(["agent", "on"])
            self._run_bt(["default-agent"])
            if bool(self._cfg.get("trust_device", True)):
                self._run_bt(["trust", device_mac], allow_fail=True)
            if bool(self._cfg.get("pair_on_connect", False)):
                self._run_bt(["pair", device_mac], allow_fail=True, timeout_s=30.0)
            self._run_bt(["connect", device_mac], allow_fail=False, timeout_s=20.0)
            if self._is_connected(device_mac):
                self._status = "connected"
                if bool(self._cfg.get("set_default_sink", True)):
                    self._set_default_sink_for_device(device_mac)
            else:
                self._status = "connect-failed"
        except Exception:
            self._status = "error"

    def _refresh_status(self) -> None:
        device_mac = self._device_mac()
        if not device_mac:
            self._status = "enabled:no-device"
            return
        if not self._is_connected(device_mac):
            self._status = "disconnected"
            return
        self._status = "connected"
        if bool(self._cfg.get("set_default_sink", True)):
            self._set_default_sink_for_device(device_mac)

    def _set_default_sink_for_device(self, device_mac: str) -> None:
        sink_name = str(self._cfg.get("sink_name", "")).strip()
        if not sink_name:
            sink_name = self._find_sink_for_mac(device_mac)
        if not sink_name:
            self._status = "connected:no-sink"
            return
        self._run(["pactl", "set-default-sink", sink_name], timeout_s=5.0)
        self._move_all_sink_inputs(sink_name)
        self._cached_sink = sink_name
        self._status = f"connected:{sink_name}"

    def _find_sink_for_mac(self, device_mac: str) -> str:
        needle = device_mac.lower().replace(":", "_")
        result = self._run(["pactl", "list", "short", "sinks"], timeout_s=5.0, allow_fail=True)
        if result.returncode != 0:
            return ""
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            sink_name = parts[1].strip()
            if needle in sink_name.lower():
                return sink_name
        return ""

    def _move_all_sink_inputs(self, sink_name: str) -> None:
        result = self._run(["pactl", "list", "short", "sink-inputs"], timeout_s=5.0, allow_fail=True)
        if result.returncode != 0:
            return
        for line in result.stdout.splitlines():
            parts = line.split()
            if not parts:
                continue
            input_id = parts[0].strip()
            if input_id.isdigit():
                self._run(["pactl", "move-sink-input", input_id, sink_name], timeout_s=3.0, allow_fail=True)

    def _is_connected(self, device_mac: str) -> bool:
        result = self._run_bt(["info", device_mac], allow_fail=True, timeout_s=5.0)
        if result.returncode != 0:
            return False
        for line in result.stdout.splitlines():
            if "Connected:" in line:
                return line.strip().lower().endswith("yes")
        return False

    def _device_mac(self) -> str:
        raw = str(self._cfg.get("device_mac", "")).strip()
        if not raw:
            return ""
        return raw.upper()

    def _run_bt(self, args: list[str], allow_fail: bool = False, timeout_s: float = 10.0) -> subprocess.CompletedProcess[str]:
        return self._run(["bluetoothctl", *args], allow_fail=allow_fail, timeout_s=timeout_s)

    def _run(
        self,
        cmd: list[str],
        allow_fail: bool = False,
        timeout_s: float = 10.0,
    ) -> subprocess.CompletedProcess[str]:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s, check=False)
        if (not allow_fail) and proc.returncode != 0:
            raise RuntimeError(f"command failed: {' '.join(cmd)}: {proc.stderr.strip()}")
        return proc
