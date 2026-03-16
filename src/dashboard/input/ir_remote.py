from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import subprocess
from typing import Any

import yaml

try:
    from evdev import InputDevice  # type: ignore
    from evdev import categorize  # type: ignore
    from evdev import ecodes  # type: ignore
except ImportError:  # pragma: no cover
    InputDevice = None  # type: ignore[assignment]
    categorize = None  # type: ignore[assignment]
    ecodes = None  # type: ignore[assignment]


_ACTION_MENU_LEFT = "menu_left"
_ACTION_MENU_RIGHT = "menu_right"
_ACTION_MENU_PRESS = "menu_press"
_ACTION_AUX_LEFT = "aux_left"
_ACTION_AUX_RIGHT = "aux_right"
_ACTION_AUX_PRESS = "aux_press"
_ACTION_VOL_UP = "volume_up"
_ACTION_VOL_DOWN = "volume_down"
_ACTION_THEME_NEXT = "theme_next"
_ACTION_MAP_TOGGLE = "map_toggle"
_ACTION_CENTER_TOGGLE = "center_toggle"
_ACTION_TUNE_UP = "tune_up"
_ACTION_TUNE_DOWN = "tune_down"

_SUPPORTED_ACTIONS = {
    _ACTION_MENU_LEFT,
    _ACTION_MENU_RIGHT,
    _ACTION_MENU_PRESS,
    _ACTION_AUX_LEFT,
    _ACTION_AUX_RIGHT,
    _ACTION_AUX_PRESS,
    _ACTION_VOL_UP,
    _ACTION_VOL_DOWN,
    _ACTION_THEME_NEXT,
    _ACTION_MAP_TOGGLE,
    _ACTION_CENTER_TOGGLE,
    _ACTION_TUNE_UP,
    _ACTION_TUNE_DOWN,
}

_ACTION_ALIASES = {
    "show_map": _ACTION_MAP_TOGGLE,
    "clock_toggle": _ACTION_CENTER_TOGGLE,
    "theme_switch": _ACTION_THEME_NEXT,
    "radio_tune_up": _ACTION_TUNE_UP,
    "radio_tune_down": _ACTION_TUNE_DOWN,
}

_PROTOCOL_ALIASES = {
    "ned": "nec",
}


@dataclass
class IrRemoteState:
    menu_presses: float = 0.0
    aux_presses: float = 0.0
    volume_up_count: float = 0.0
    volume_down_count: float = 0.0
    theme_next_count: float = 0.0
    map_toggle_count: float = 0.0
    center_toggle_count: float = 0.0
    tune_up_count: float = 0.0
    tune_down_count: float = 0.0
    diagnostics_open_count: float = 0.0


class IrRemoteInput:
    """Maps Linux IR key events to rotary-style dashboard actions."""

    def __init__(self, spec: dict[str, Any]) -> None:
        self._spec = spec
        self._state = IrRemoteState()
        self._ready = False
        self._allow_repeats = bool(spec.get("allow_repeats", False))
        self._event_devices: list[Any] = []
        self._device_path = ""
        self._device_name = ""
        self._last_reopen_attempt_s = 0.0
        self._reopen_interval_s = max(0.5, float(spec.get("reopen_interval_s", 2.0)))
        self._last_event_seen_s = 0.0
        self._stale_reopen_s = max(2.0, float(spec.get("stale_reopen_s", 8.0)))
        self._key_action_map: dict[str, str] = {}
        self._last_code: str = ""
        self._diag_target_key = ""
        self._diag_count = 0
        self._diag_start_s = 0.0
        self._diag_last_s = 0.0
        self._diag_threshold = max(2, int(spec.get("diagnostics_pulse_count", 10)))
        self._diag_window_s = max(0.2, float(spec.get("diagnostics_window_ms", 1500.0)) / 1000.0)
        self._diag_gap_s = max(0.02, float(spec.get("diagnostics_max_gap_ms", 180.0)) / 1000.0)

        if not bool(spec.get("enabled", False)):
            return
        self._configure_kernel_protocols()
        if InputDevice is None or ecodes is None:
            return

        profile_path_raw = str(spec.get("profile_path", "")).strip()
        if profile_path_raw:
            self._key_action_map = self._load_profile(profile_path_raw)

        self._device_path = str(spec.get("device_path", "")).strip()
        self._device_name = str(spec.get("device_name", "")).strip()
        self._event_devices = self._open_devices(device_path=self._device_path, device_name=self._device_name)
        if not self._event_devices:
            return
        self._ready = True
        import time
        self._last_event_seen_s = time.monotonic()

    def _normalize_protocol_name(self, raw: Any) -> str:
        text = str(raw).strip().lower()
        if not text:
            return ""
        return _PROTOCOL_ALIASES.get(text, text)

    def _requested_protocols(self) -> list[str]:
        # Support both single `protocol` and list-based `protocols` config.
        raw_protocols = self._spec.get("protocols")
        values: list[str]
        if isinstance(raw_protocols, str):
            values = [raw_protocols]
        elif isinstance(raw_protocols, list):
            values = [str(item) for item in raw_protocols]
        else:
            single = self._spec.get("protocol", "")
            values = [str(single)] if single else []
        protocols: list[str] = []
        for candidate in values:
            normalized = self._normalize_protocol_name(candidate)
            if normalized and normalized not in protocols:
                protocols.append(normalized)
        return protocols

    def _configure_kernel_protocols(self) -> None:
        if not bool(self._spec.get("apply_protocol", True)):
            return
        protocols = self._requested_protocols()
        if not protocols:
            return
        cmd = ["ir-keytable"]
        rc_device = str(self._spec.get("rc_device", "")).strip()
        if rc_device:
            cmd.extend(["-s", rc_device])
        for protocol in protocols:
            cmd.extend(["-p", protocol])
        try:
            subprocess.run(cmd, check=False, capture_output=True, text=True)
        except OSError:
            # Continue without hard-failing if ir-keytable is unavailable.
            return

    def poll(self) -> dict[str, float]:
        updates = {
            "inputs.rotary.menu.delta": 0.0,
            "inputs.rotary.menu.presses": self._state.menu_presses,
            "inputs.rotary.aux.delta": 0.0,
            "inputs.rotary.aux.presses": self._state.aux_presses,
            "inputs.ir.command.volume_up_count": self._state.volume_up_count,
            "inputs.ir.command.volume_down_count": self._state.volume_down_count,
            "inputs.ir.command.theme_next_count": self._state.theme_next_count,
            "inputs.ir.command.map_toggle_count": self._state.map_toggle_count,
            "inputs.ir.command.center_toggle_count": self._state.center_toggle_count,
            "inputs.ir.command.tune_up_count": self._state.tune_up_count,
            "inputs.ir.command.tune_down_count": self._state.tune_down_count,
            "inputs.ir.command.diagnostics_open_count": self._state.diagnostics_open_count,
        }
        if not self._ready or not self._event_devices or categorize is None or ecodes is None:
            self._maybe_reopen_devices()
            return updates

        live_devices: list[Any] = []
        saw_event = False
        for device in self._event_devices:
            try:
                events = device.read()
            except BlockingIOError:
                live_devices.append(device)
                continue
            except OSError:
                continue
            live_devices.append(device)
            try:
                for event in events:
                    if event.type != ecodes.EV_KEY:
                        continue
                    saw_event = True
                    key_event = categorize(event)
                    key_name = str(key_event.keycode)
                    pulse = key_event.keystate in (key_event.key_down, key_event.key_hold)
                    if pulse:
                        self._last_code = key_name
                        self._track_diagnostics_pulse(key_name)
                    action = self._key_action_map.get(key_name)
                    if not action:
                        continue
                    if key_event.keystate == key_event.key_down:
                        self._apply_action(action, updates)
                    elif self._allow_repeats and key_event.keystate == key_event.key_hold:
                        self._apply_action(action, updates)
            except BlockingIOError:
                # Some evdev versions raise while consuming the iterator itself.
                pass
        self._event_devices = live_devices
        if not self._event_devices:
            self._ready = False
            self._maybe_reopen_devices()
        else:
            import time
            now_s = time.monotonic()
            if (now_s - self._last_event_seen_s) >= self._stale_reopen_s:
                self._ready = False
                self._event_devices = []
                self._maybe_reopen_devices()
        if not saw_event:
            return updates

        import time
        self._last_event_seen_s = time.monotonic()
        return updates

    def _maybe_reopen_devices(self) -> None:
        import time

        if InputDevice is None or ecodes is None:
            return
        if self._ready and self._event_devices:
            return
        now_s = time.monotonic()
        if (now_s - self._last_reopen_attempt_s) < self._reopen_interval_s:
            return
        self._last_reopen_attempt_s = now_s
        reopened = self._open_devices(device_path=self._device_path, device_name=self._device_name)
        if reopened:
            self._event_devices = reopened
            self._ready = True
            self._last_event_seen_s = now_s

    def _apply_action(self, action: str, updates: dict[str, float]) -> None:
        if action == _ACTION_MENU_LEFT:
            updates["inputs.rotary.menu.delta"] -= 1.0
            return
        if action == _ACTION_MENU_RIGHT:
            updates["inputs.rotary.menu.delta"] += 1.0
            return
        if action == _ACTION_MENU_PRESS:
            self._state.menu_presses += 1.0
            updates["inputs.rotary.menu.presses"] = self._state.menu_presses
            return
        if action == _ACTION_AUX_LEFT:
            updates["inputs.rotary.aux.delta"] -= 1.0
            return
        if action == _ACTION_AUX_RIGHT:
            updates["inputs.rotary.aux.delta"] += 1.0
            return
        if action == _ACTION_AUX_PRESS:
            self._state.aux_presses += 1.0
            updates["inputs.rotary.aux.presses"] = self._state.aux_presses
            return
        if action == _ACTION_VOL_UP:
            self._state.volume_up_count += 1.0
            updates["inputs.ir.command.volume_up_count"] = self._state.volume_up_count
            return
        if action == _ACTION_VOL_DOWN:
            self._state.volume_down_count += 1.0
            updates["inputs.ir.command.volume_down_count"] = self._state.volume_down_count
            return
        if action == _ACTION_THEME_NEXT:
            self._state.theme_next_count += 1.0
            updates["inputs.ir.command.theme_next_count"] = self._state.theme_next_count
            return
        if action == _ACTION_MAP_TOGGLE:
            self._state.map_toggle_count += 1.0
            updates["inputs.ir.command.map_toggle_count"] = self._state.map_toggle_count
            return
        if action == _ACTION_CENTER_TOGGLE:
            self._state.center_toggle_count += 1.0
            updates["inputs.ir.command.center_toggle_count"] = self._state.center_toggle_count
            return
        if action == _ACTION_TUNE_UP:
            self._state.tune_up_count += 1.0
            updates["inputs.ir.command.tune_up_count"] = self._state.tune_up_count
            return
        if action == _ACTION_TUNE_DOWN:
            self._state.tune_down_count += 1.0
            updates["inputs.ir.command.tune_down_count"] = self._state.tune_down_count

    @property
    def last_code(self) -> str:
        return self._last_code

    def _track_diagnostics_pulse(self, key_name: str) -> None:
        import time

        now_s = time.monotonic()
        # Treat diagnostics counter as a live pulse activity indicator.
        self._state.diagnostics_open_count += 1.0
        if (
            key_name == self._diag_target_key
            and (now_s - self._diag_start_s) <= self._diag_window_s
            and (now_s - self._diag_last_s) <= self._diag_gap_s
        ):
            self._diag_count += 1
        else:
            self._diag_target_key = key_name
            self._diag_count = 1
            self._diag_start_s = now_s
        self._diag_last_s = now_s

        if self._diag_count >= self._diag_threshold:
            self._diag_target_key = ""
            self._diag_count = 0
            self._diag_start_s = 0.0
            self._diag_last_s = 0.0

    def _load_profile(self, profile_path: str) -> dict[str, str]:
        path = Path(profile_path)
        if not path.exists():
            return {}
        try:
            raw = yaml.safe_load(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}
        if not isinstance(raw, dict):
            return {}

        remote_cfg = raw.get("remote", {})
        if not isinstance(remote_cfg, dict):
            return {}
        mappings = remote_cfg.get("mappings", {})
        controls = remote_cfg.get("controls", {})

        parsed: dict[str, str] = {}
        if isinstance(mappings, dict):
            for key_name, action_name in mappings.items():
                if not isinstance(key_name, str) or not isinstance(action_name, str):
                    continue
                key_name_norm = key_name.strip().upper()
                action_name_norm = self._normalize_action_name(action_name)
                if not key_name_norm or not action_name_norm:
                    continue
                parsed[key_name_norm] = action_name_norm
        if isinstance(controls, dict):
            for action_name, key_name in controls.items():
                if not isinstance(action_name, str):
                    continue
                action_name_norm = self._normalize_action_name(action_name)
                if not action_name_norm:
                    continue
                if not isinstance(key_name, str) or not key_name.strip():
                    continue
                parsed[key_name.strip().upper()] = action_name_norm
        return parsed

    def _normalize_action_name(self, action_name: str) -> str:
        normalized = action_name.strip().lower()
        if normalized in _ACTION_ALIASES:
            normalized = _ACTION_ALIASES[normalized]
        if normalized not in _SUPPORTED_ACTIONS:
            return ""
        return normalized

    def _open_devices(self, device_path: str, device_name: str) -> list[Any]:
        assert InputDevice is not None
        if device_path:
            try:
                device = InputDevice(device_path)
                self._set_device_nonblocking(device)
                return [device]
            except OSError:
                return []

        if not device_name:
            return []
        try:
            from evdev import list_devices  # type: ignore
        except ImportError:
            return []
        matches: list[Any] = []
        for candidate_path in list_devices():
            try:
                candidate = InputDevice(candidate_path)
            except OSError:
                continue
            if str(candidate.name).strip() == device_name:
                self._set_device_nonblocking(candidate)
                matches.append(candidate)
                continue
            try:
                candidate.close()
            except OSError:
                pass
        return matches

    def _set_device_nonblocking(self, device: Any) -> None:
        """Handle evdev API differences across distro versions."""
        if hasattr(device, "set_blocking"):
            device.set_blocking(False)
            return
        fd = getattr(device, "fd", None)
        if isinstance(fd, int):
            os.set_blocking(fd, False)
