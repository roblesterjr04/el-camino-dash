from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
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
        if InputDevice is None or ecodes is None:
            return

        profile_path_raw = str(spec.get("profile_path", "")).strip()
        if not profile_path_raw:
            return
        self._key_action_map = self._load_profile(profile_path_raw)
        if not self._key_action_map:
            return

        device_path = str(spec.get("device_path", "")).strip()
        device_name = str(spec.get("device_name", "")).strip()
        self._event_devices = self._open_devices(device_path=device_path, device_name=device_name)
        if not self._event_devices:
            return
        self._ready = True

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
            for event in events:
                if event.type != ecodes.EV_KEY:
                    continue
                saw_event = True
                key_event = categorize(event)
                key_name = str(key_event.keycode)
                pulse = key_event.keystate == key_event.key_down or (
                    self._allow_repeats and key_event.keystate == key_event.key_hold
                )
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
        self._event_devices = live_devices
        if not self._event_devices:
            self._ready = False
        if not saw_event:
            return updates

        return updates

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
            self._state.diagnostics_open_count += 1.0
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
                device.set_blocking(False)
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
                candidate.set_blocking(False)
                matches.append(candidate)
                continue
            try:
                candidate.close()
            except OSError:
                pass
        return matches
