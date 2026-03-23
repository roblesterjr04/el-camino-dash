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
        self._toggle_play_next = False
        self._player_state = ""
        self._track_title = ""
        self._track_artist = ""
        self._active_device_mac = ""
        self._last_reconnect_attempt_s = 0.0
        self._loopback_module_id = ""

    def status(self) -> str:
        if not self._cfg.get("enabled", False):
            return "disabled"
        return self._status

    def media_status(self) -> str:
        if not self._cfg.get("enabled", False):
            return "disabled"
        if self._status.startswith("connected"):
            return self._player_state or "connected"
        return self._status

    def media_track(self) -> str:
        if not self._cfg.get("enabled", False):
            return "disabled"
        if self._status.startswith("connected"):
            if self._track_artist and self._track_title:
                return f"{self._track_artist} - {self._track_title}"
            if self._track_title:
                return self._track_title
            if self._track_artist:
                return self._track_artist
            return "-"
        return self._status

    def media_title(self) -> str:
        if not self._cfg.get("enabled", False) or not self._status.startswith("connected"):
            return ""
        return self._track_title or ""

    def media_artist(self) -> str:
        if not self._cfg.get("enabled", False) or not self._status.startswith("connected"):
            return ""
        return self._track_artist or ""

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
        if bool(self._cfg.get("auto_connect", True)) and not self._status.startswith("connected"):
            retry_interval_s = max(3.0, float(self._cfg.get("reconnect_interval_s", 8.0)))
            if (now - self._last_reconnect_attempt_s) >= retry_interval_s:
                self._last_reconnect_attempt_s = now
                self.reconnect()

    def reconnect(self) -> None:
        if not self._cfg.get("enabled", False):
            self._status = "disabled"
            return
        device_mac = self._target_device_mac_for_connect()
        if not device_mac:
            self._status = "enabled:no-device"
            return
        try:
            session_cmds = ["power on", "agent on", "default-agent"]
            if bool(self._cfg.get("trust_device", True)):
                session_cmds.append(f"trust {device_mac}")
            if bool(self._cfg.get("pair_on_connect", False)):
                session_cmds.append(f"pair {device_mac}")
            session_cmds.append(f"connect {device_mac}")
            self._run_bt_session(session_cmds, timeout_s=30.0)
            if self._is_connected(device_mac):
                self._active_device_mac = device_mac
                self._status = "connected"
                self._ensure_a2dp_profile(device_mac)
                if bool(self._cfg.get("set_default_sink", True)):
                    self._set_default_sink_for_device(device_mac)
            else:
                self._status = "connect-failed"
        except Exception:
            self._status = "error"

    def media_play_pause(self) -> str:
        # Prefer native toggle; fallback to alternating play/pause on stacks
        # where play-pause is unsupported.
        result = self._run_player_action("play-pause")
        if not result.startswith("player-") or not result.endswith("-failed"):
            return result
        fallback_action = "play" if self._toggle_play_next else "pause"
        fallback_result = self._run_player_action(fallback_action)
        if not fallback_result.startswith("player-") or not fallback_result.endswith("-failed"):
            self._toggle_play_next = not self._toggle_play_next
        return fallback_result

    def media_next(self) -> str:
        return self._run_player_action("next")

    def media_stop(self) -> str:
        return self._run_player_action("stop")

    def set_media_volume(self, volume: float) -> str:
        if not self._cfg.get("enabled", False):
            return "disabled"
        clamped = max(0.0, min(1.0, float(volume)))
        device_mac = self._resolved_device_mac()
        if not device_mac:
            return "enabled:no-device"
        sink_name = self._cached_sink or self._find_sink_for_mac(device_mac)
        if not sink_name:
            self._refresh_status()
            sink_name = self._cached_sink or self._find_sink_for_mac(device_mac)
        if not sink_name:
            self._status = "connected:no-sink"
            return self._status
        percent = int(round(clamped * 100.0))
        try:
            # Apply to sink and active sink-input streams for wider compatibility.
            self._run(["pactl", "set-sink-volume", sink_name, f"{percent}%"], timeout_s=5.0)
            self._set_sink_inputs_volume(sink_name=sink_name, percent=percent)
            self._status = f"connected:{sink_name}:vol={percent}%"
            return self._status
        except Exception:
            self._refresh_status()
            return self._status

    def _refresh_status(self) -> None:
        device_mac = self._resolved_device_mac()
        if not device_mac:
            self._status = "enabled:no-device"
            self._player_state = ""
            self._track_title = ""
            self._track_artist = ""
            return
        if not self._is_connected(device_mac):
            self._status = "disconnected"
            self._player_state = ""
            self._track_title = ""
            self._track_artist = ""
            return
        was_connected = self._status.startswith("connected")
        self._active_device_mac = device_mac
        self._status = "connected"
        if (not was_connected) or (not self._cached_sink):
            # Profile/sink routing calls can be expensive; avoid repeating
            # them on every poll once already connected.
            self._ensure_a2dp_profile(device_mac)
            if bool(self._cfg.get("set_default_sink", True)):
                self._set_default_sink_for_device(device_mac)
        self._refresh_player_metadata()

    def _set_default_sink_for_device(self, device_mac: str) -> None:
        target_sink = str(self._cfg.get("sink_name", "")).strip() or self._default_sink_name()
        bt_sink = self._find_sink_for_mac(device_mac)
        if not bt_sink:
            # Try to force A2DP sink profile then rediscover.
            self._ensure_a2dp_profile(device_mac)
            bt_sink = self._find_sink_for_mac(device_mac)
        if bt_sink:
            self._run(["pactl", "set-default-sink", bt_sink], timeout_s=5.0)
            self._move_all_sink_inputs(bt_sink)
            self._cached_sink = bt_sink
            self._status = f"connected:{bt_sink}"
            return

        # iPhone/phone source mode: create/refresh loopback from bluez_source
        # into desired playback sink (DAC/HDMI).
        bt_source = self._find_source_for_mac(device_mac)
        if bt_source:
            if target_sink:
                self._run(["pactl", "set-default-sink", target_sink], timeout_s=5.0, allow_fail=True)
                self._ensure_loopback(source_name=bt_source, sink_name=target_sink)
                self._cached_sink = target_sink
                self._status = f"connected:{bt_source}->{target_sink}"
                return
            self._status = f"connected:{bt_source}:no-playback-sink"
            return

        self._status = "connected:no-sink"

    def _ensure_a2dp_profile(self, device_mac: str) -> None:
        card_name = self._find_card_for_mac(device_mac)
        if not card_name:
            return
        # Prefer highest-quality sink profile available.
        for profile in ("a2dp-sink-sbc_xq", "a2dp_sink"):
            self._run(
                ["pactl", "set-card-profile", card_name, profile],
                timeout_s=5.0,
                allow_fail=True,
            )
            sink_name = self._find_sink_for_mac(device_mac)
            if sink_name:
                return

    def _find_card_for_mac(self, device_mac: str) -> str:
        needle = device_mac.lower().replace(":", "_")
        result = self._run(["pactl", "list", "short", "cards"], timeout_s=5.0, allow_fail=True)
        if result.returncode != 0:
            return ""
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            card_name = parts[1].strip()
            if needle in card_name.lower():
                return card_name
        return ""

    def _refresh_player_metadata(self) -> None:
        self._player_state = ""
        self._track_title = ""
        self._track_artist = ""
        result = self._run_bt(["player.show"], allow_fail=True, timeout_s=5.0)
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

    def _run_player_action(self, action: str) -> str:
        if not self._cfg.get("enabled", False):
            return "disabled"
        result = self._run_bt([f"player.{action}"], allow_fail=True, timeout_s=5.0)
        if result.returncode == 0:
            self._refresh_status()
            return self._status
        # Retry after reconnect for transient bluez state.
        self._refresh_status()
        if not self._status.startswith("connected"):
            self.reconnect()
        result = self._run_bt([f"player.{action}"], allow_fail=True, timeout_s=5.0)
        if result.returncode == 0:
            self._refresh_status()
            return self._status
        return f"player-{action}-failed"

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

    def _find_source_for_mac(self, device_mac: str) -> str:
        needle = device_mac.lower().replace(":", "_")
        result = self._run(["pactl", "list", "short", "sources"], timeout_s=5.0, allow_fail=True)
        if result.returncode != 0:
            return ""
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            source_name = parts[1].strip()
            if needle in source_name.lower():
                return source_name
        return ""

    def _default_sink_name(self) -> str:
        result = self._run(["pactl", "info"], timeout_s=5.0, allow_fail=True)
        if result.returncode != 0:
            return ""
        for line in result.stdout.splitlines():
            line = line.strip()
            if line.startswith("Default Sink:"):
                return line.split(":", 1)[1].strip()
        return ""

    def _find_loopback_module_for_source(self, source_name: str, sink_name: str = "") -> str:
        result = self._run(["pactl", "list", "short", "modules"], timeout_s=5.0, allow_fail=True)
        if result.returncode != 0:
            return ""
        for line in result.stdout.splitlines():
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            module_id = parts[0].strip()
            module_name = parts[1].strip()
            args = parts[2].strip()
            if module_name != "module-loopback" or source_name not in args:
                continue
            if sink_name and f"sink={sink_name}" not in args:
                continue
            if f"source={source_name}" not in args:
                continue
            return module_id
        return ""

    def _ensure_loopback(self, source_name: str, sink_name: str) -> None:
        # Keep the existing loopback if it already targets this exact sink.
        existing = self._find_loopback_module_for_source(source_name, sink_name)
        if existing:
            self._loopback_module_id = existing
            return
        # Source exists but sink changed: replace the old loopback.
        existing = self._find_loopback_module_for_source(source_name)
        if existing:
            self._run(["pactl", "unload-module", existing], timeout_s=5.0, allow_fail=True)
        result = self._run(
            ["pactl", "load-module", "module-loopback", f"source={source_name}", f"sink={sink_name}"],
            timeout_s=5.0,
            allow_fail=True,
        )
        if result.returncode == 0:
            self._loopback_module_id = result.stdout.strip()

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

    def _set_sink_inputs_volume(self, sink_name: str, percent: int) -> None:
        sinks = self._run(["pactl", "list", "short", "sinks"], timeout_s=5.0, allow_fail=True)
        if sinks.returncode != 0:
            return
        sink_index = ""
        for line in sinks.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[1].strip() == sink_name and parts[0].isdigit():
                sink_index = parts[0].strip()
                break
        if not sink_index:
            return
        inputs = self._run(["pactl", "list", "short", "sink-inputs"], timeout_s=5.0, allow_fail=True)
        if inputs.returncode != 0:
            return
        for line in inputs.stdout.splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            input_id, input_sink = parts[0].strip(), parts[1].strip()
            if input_id.isdigit() and input_sink == sink_index:
                self._run(["pactl", "set-sink-input-volume", input_id, f"{percent}%"], timeout_s=3.0, allow_fail=True)

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

    def _connected_device_macs(self) -> list[str]:
        result = self._run_bt(["devices", "Connected"], allow_fail=True, timeout_s=5.0)
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

    def _paired_device_macs(self) -> list[str]:
        result = self._run_bt(["paired-devices"], allow_fail=True, timeout_s=5.0)
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

    def _resolved_device_mac(self) -> str:
        configured = self._device_mac()
        if configured and self._is_connected(configured):
            return configured
        for mac in self._connected_device_macs():
            if self._is_connected(mac):
                return mac
        if configured:
            return configured
        if self._active_device_mac:
            return self._active_device_mac
        return ""

    def _target_device_mac_for_connect(self) -> str:
        configured = self._device_mac()
        if configured:
            return configured
        connected = self._connected_device_macs()
        if connected:
            return connected[0]
        paired = self._paired_device_macs()
        if paired:
            return paired[0]
        return ""

    def _run_bt(self, args: list[str], allow_fail: bool = False, timeout_s: float = 10.0) -> subprocess.CompletedProcess[str]:
        return self._run(["bluetoothctl", *args], allow_fail=allow_fail, timeout_s=timeout_s)

    def _run_bt_session(self, commands: list[str], timeout_s: float = 20.0) -> subprocess.CompletedProcess[str]:
        script = "\n".join([cmd.strip() for cmd in commands if cmd.strip()] + ["quit"]) + "\n"
        return self._run(["bluetoothctl"], allow_fail=True, timeout_s=timeout_s, stdin_data=script)

    def _run(
        self,
        cmd: list[str],
        allow_fail: bool = False,
        timeout_s: float = 10.0,
        stdin_data: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        effective_timeout = max(0.2, float(self._cfg.get("command_timeout_s", timeout_s)))
        try:
            proc = subprocess.run(
                cmd,
                input=stdin_data,
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
