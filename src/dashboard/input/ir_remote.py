from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import re
import subprocess
import time
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
_ACTION_DASH_EXIT = "dash_exit"
_ACTION_MENU_OPEN = "menu_open"

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
    _ACTION_DASH_EXIT,
    _ACTION_MENU_OPEN,
}

_ACTION_ALIASES = {
    "show_map": _ACTION_MAP_TOGGLE,
    "clock_toggle": _ACTION_CENTER_TOGGLE,
    "theme_switch": _ACTION_THEME_NEXT,
    "radio_tune_up": _ACTION_TUNE_UP,
    "radio_tune_down": _ACTION_TUNE_DOWN,
    "menu_exit": _ACTION_DASH_EXIT,
    "exit": _ACTION_DASH_EXIT,
    "home": _ACTION_DASH_EXIT,
    "main_screen": _ACTION_DASH_EXIT,
    "menu_open_back": _ACTION_MENU_OPEN,
    "menu_open": _ACTION_MENU_OPEN,
    "menu": _ACTION_MENU_OPEN,
    "back": _ACTION_MENU_OPEN,
}

_PROTOCOL_ALIASES = {
    "ned": "nec",
}

_DEFAULT_KEY_ACTION_MAP = {
    # Primary menu/navigation keys.
    "KEY_LEFT": _ACTION_MENU_LEFT,
    "KEY_RIGHT": _ACTION_MENU_RIGHT,
    "KEY_UP": _ACTION_MENU_RIGHT,
    "KEY_DOWN": _ACTION_MENU_LEFT,
    "KEY_OK": _ACTION_MENU_PRESS,
    "KEY_ENTER": _ACTION_MENU_PRESS,
    "KEY_SELECT": _ACTION_MENU_PRESS,
    # Audio/media-style keys commonly present on remotes.
    "KEY_VOLUMEUP": _ACTION_VOL_UP,
    "KEY_VOLUMEDOWN": _ACTION_VOL_DOWN,
    "KEY_CHANNELUP": _ACTION_TUNE_UP,
    "KEY_CHANNELDOWN": _ACTION_TUNE_DOWN,
    "KEY_NEXT": _ACTION_THEME_NEXT,
    "KEY_PREVIOUS": _ACTION_CENTER_TOGGLE,
    "KEY_INFO": _ACTION_MAP_TOGGLE,
    "KEY_EXIT": _ACTION_DASH_EXIT,
    "KEY_BACK": _ACTION_MENU_OPEN,
    "KEY_HOME": _ACTION_DASH_EXIT,
    "KEY_MENU": _ACTION_MENU_OPEN,
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
    dash_exit_count: float = 0.0
    menu_open_count: float = 0.0
    diagnostics_open_count: float = 0.0


class IrRemoteInput:
    """Maps Linux IR key events to rotary-style dashboard actions."""

    def __init__(self, spec: dict[str, Any]) -> None:
        self._spec = spec
        self._state = IrRemoteState()
        self._ready = False
        self._log_path = self._boot_log_path("elc-ir-input.log")
        self._last_log_s = 0.0
        self._allow_repeats = bool(spec.get("allow_repeats", False))
        self._event_devices: list[Any] = []
        self._device_path = ""
        self._device_name = ""
        self._last_reopen_attempt_s = 0.0
        self._reopen_interval_s = max(0.5, float(spec.get("reopen_interval_s", 2.0)))
        self._last_event_seen_s = 0.0
        self._stale_reopen_s = max(2.0, float(spec.get("stale_reopen_s", 8.0)))
        self._debounce_s = max(0.0, float(spec.get("debounce_ms", 180.0)) / 1000.0)
        self._repeat_debounce_s = max(0.0, float(spec.get("repeat_debounce_ms", 130.0)) / 1000.0)
        self._last_emit_s_by_source: dict[str, float] = {}
        self._key_action_map: dict[str, str] = {}
        self._scan_action_map: dict[str, str] = {}
        self._last_code: str = ""
        self._diag_target_key = ""
        self._diag_count = 0
        self._diag_start_s = 0.0
        self._diag_last_s = 0.0
        self._diag_threshold = max(2, int(spec.get("diagnostics_pulse_count", 10)))
        self._diag_window_s = max(0.2, float(spec.get("diagnostics_window_ms", 1500.0)) / 1000.0)
        self._diag_gap_s = max(0.02, float(spec.get("diagnostics_max_gap_ms", 180.0)) / 1000.0)
        self._any_key_action = self._normalize_action_name(str(spec.get("any_key_action", "menu_press")))

        if not bool(spec.get("enabled", False)):
            return
        self._configure_kernel_protocols()
        if InputDevice is None or ecodes is None:
            self._append_log("ir disabled: evdev unavailable")
            return

        profile_path_raw = str(spec.get("profile_path", "")).strip()
        if profile_path_raw:
            self._key_action_map, self._scan_action_map = self._load_profile(profile_path_raw)
        if not self._key_action_map and not self._scan_action_map:
            self._key_action_map = dict(_DEFAULT_KEY_ACTION_MAP)
            self._append_log("ir profile empty; using default key mappings")

        self._device_path = str(spec.get("device_path", "")).strip()
        self._device_name = str(spec.get("device_name", "")).strip()
        self._event_devices = self._open_devices(device_path=self._device_path, device_name=self._device_name)
        if not self._event_devices:
            self._append_log("ir input devices not found at startup")
            return
        self._ready = True
        self._last_event_seen_s = time.monotonic()
        self._append_log(f"ir ready: devices={len(self._event_devices)}")

    @staticmethod
    def _boot_log_path(filename: str) -> str:
        base = "/boot/firmware" if os.path.isdir("/boot/firmware") else "/boot"
        return f"{base}/{filename}"

    def _append_log(self, message: str) -> None:
        line = f"[ir-input] {message}"
        print(line, flush=True)
        try:
            ts = time.strftime("%Y-%m-%dT%H:%M:%S%z")
            with open(self._log_path, "a", encoding="utf-8") as handle:
                handle.write(f"[{ts}] {message}\n")
        except OSError:
            pass

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

    def _discover_rc_device_by_name(self, device_name: str) -> str:
        name = device_name.strip()
        if not name:
            return ""
        try:
            result = subprocess.run(["ir-keytable"], check=False, capture_output=True, text=True)
        except OSError:
            return ""
        if result.returncode != 0:
            return ""

        rc_id = ""
        found_name = ""
        for line in result.stdout.splitlines():
            match_rc = re.search(r"/sys/class/rc/(rc\d+)/", line)
            if match_rc:
                rc_id = match_rc.group(1)
                found_name = ""
                continue
            if "Name:" in line:
                found_name = line.split("Name:", 1)[1].strip()
                if found_name == name and rc_id:
                    return rc_id
        return ""

    def _configure_kernel_protocols(self) -> None:
        if not bool(self._spec.get("apply_protocol", True)):
            return
        protocols = self._requested_protocols()
        if not protocols:
            return
        cmd = ["ir-keytable"]
        rc_device = str(self._spec.get("rc_device", "")).strip()
        if not rc_device:
            rc_device = self._discover_rc_device_by_name(str(self._spec.get("device_name", "")))
        if rc_device:
            cmd.extend(["-s", rc_device])
        for protocol in protocols:
            cmd.extend(["-p", protocol])
        try:
            result = subprocess.run(cmd, check=False, capture_output=True, text=True)
            detail = result.stdout.strip() or result.stderr.strip()
            self._append_log(
                f"ir-keytable rc={result.returncode} cmd={' '.join(cmd)} detail={detail[:180]}"
            )
        except OSError:
            # Continue without hard-failing if ir-keytable is unavailable.
            self._append_log("ir-keytable unavailable")
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
            "inputs.ir.command.dash_exit_count": self._state.dash_exit_count,
            "inputs.ir.command.menu_open_count": self._state.menu_open_count,
            "inputs.ir.command.diagnostics_open_count": self._state.diagnostics_open_count,
        }
        if not self._ready or not self._event_devices or categorize is None or ecodes is None:
            self._maybe_reopen_devices()
            return updates

        live_devices: list[Any] = []
        saw_event = False
        saw_read_success = False
        for device in self._event_devices:
            try:
                events = device.read()
            except BlockingIOError:
                live_devices.append(device)
                saw_read_success = True
                continue
            except OSError:
                if (time.monotonic() - self._last_log_s) >= 2.0:
                    self._append_log("ir device read OSError; dropping device")
                    self._last_log_s = time.monotonic()
                continue
            live_devices.append(device)
            saw_read_success = True
            try:
                pending_scan_actions: list[tuple[str, str]] = []
                for event in events:
                    if event.type == ecodes.EV_MSC and event.code == ecodes.MSC_SCAN:
                        scan_name = self._normalize_scancode(event.value)
                        if not scan_name:
                            continue
                        self._last_code = f"SCAN:{scan_name}"
                        action_from_scan = self._scan_action_map.get(scan_name, "")
                        if action_from_scan:
                            pending_scan_actions.append((scan_name, action_from_scan))
                        continue
                    if event.type != ecodes.EV_KEY:
                        continue
                    saw_event = True
                    key_event = categorize(event)
                    key_names = self._candidate_key_names(key_event.keycode)
                    key_name = key_names[0] if key_names else str(key_event.keycode).strip().upper()
                    pulse = key_event.keystate in (key_event.key_down, key_event.key_hold)
                    if pulse:
                        self._last_code = key_name
                        self._track_diagnostics_pulse(key_name)
                    pending_scan_action = pending_scan_actions.pop(0) if pending_scan_actions else ("", "")
                    action = self._resolve_action(key_names)
                    if (not action) and pending_scan_action[1]:
                        action = pending_scan_action[1]
                        if pulse:
                            self._last_code = f"SCAN:{pending_scan_action[0]}"
                            self._track_diagnostics_pulse(self._last_code)
                    if not action and self._any_key_action and pulse and key_event.keystate == key_event.key_down:
                        action = self._any_key_action
                        self._append_log(f"ir fallback action: {action} from key={key_name}")
                    if not action:
                        continue
                    if key_event.keystate == key_event.key_down:
                        if self._should_emit_action(source_id=f"key:{key_name}", is_repeat=False):
                            self._apply_action(action, updates)
                    elif self._allow_repeats and key_event.keystate == key_event.key_hold:
                        if self._should_emit_action(source_id=f"key:{key_name}", is_repeat=True):
                            self._apply_action(action, updates)
                # Some remotes produce decodable scan events without key mappings.
                # Trigger mapped actions directly from remaining scans.
                for scan_name, action in pending_scan_actions:
                    self._last_code = f"SCAN:{scan_name}"
                    self._track_diagnostics_pulse(self._last_code)
                    if self._should_emit_action(source_id=f"scan:{scan_name}", is_repeat=False):
                        self._apply_action(action, updates)
            except BlockingIOError:
                # Some evdev versions raise while consuming the iterator itself.
                pass
        self._event_devices = live_devices
        if not self._event_devices:
            self._ready = False
            self._maybe_reopen_devices()
        else:
            now_s = time.monotonic()
            if saw_read_success:
                self._last_event_seen_s = now_s
            elif (now_s - self._last_event_seen_s) >= self._stale_reopen_s:
                self._ready = False
                self._event_devices = []
                self._maybe_reopen_devices()
        if not saw_event:
            return updates

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
            self._append_log(f"ir devices reopened: count={len(reopened)}")

    def _apply_action(self, action: str, updates: dict[str, float]) -> None:
        if action == _ACTION_MENU_LEFT:
            updates["inputs.rotary.menu.delta"] -= 1.0
            self._append_log("ir action: menu_left")
            return
        if action == _ACTION_MENU_RIGHT:
            updates["inputs.rotary.menu.delta"] += 1.0
            self._append_log("ir action: menu_right")
            return
        if action == _ACTION_MENU_PRESS:
            self._state.menu_presses += 1.0
            updates["inputs.rotary.menu.presses"] = self._state.menu_presses
            self._append_log("ir action: menu_press")
            return
        if action == _ACTION_AUX_LEFT:
            updates["inputs.rotary.aux.delta"] -= 1.0
            self._append_log("ir action: aux_left")
            return
        if action == _ACTION_AUX_RIGHT:
            updates["inputs.rotary.aux.delta"] += 1.0
            self._append_log("ir action: aux_right")
            return
        if action == _ACTION_AUX_PRESS:
            self._state.aux_presses += 1.0
            updates["inputs.rotary.aux.presses"] = self._state.aux_presses
            self._append_log("ir action: aux_press")
            return
        if action == _ACTION_VOL_UP:
            self._state.volume_up_count += 1.0
            updates["inputs.ir.command.volume_up_count"] = self._state.volume_up_count
            self._append_log("ir action: volume_up")
            return
        if action == _ACTION_VOL_DOWN:
            self._state.volume_down_count += 1.0
            updates["inputs.ir.command.volume_down_count"] = self._state.volume_down_count
            self._append_log("ir action: volume_down")
            return
        if action == _ACTION_THEME_NEXT:
            self._state.theme_next_count += 1.0
            updates["inputs.ir.command.theme_next_count"] = self._state.theme_next_count
            self._append_log("ir action: theme_next")
            return
        if action == _ACTION_MAP_TOGGLE:
            self._state.map_toggle_count += 1.0
            updates["inputs.ir.command.map_toggle_count"] = self._state.map_toggle_count
            self._append_log("ir action: map_toggle")
            return
        if action == _ACTION_CENTER_TOGGLE:
            self._state.center_toggle_count += 1.0
            updates["inputs.ir.command.center_toggle_count"] = self._state.center_toggle_count
            self._append_log("ir action: center_toggle")
            return
        if action == _ACTION_TUNE_UP:
            self._state.tune_up_count += 1.0
            updates["inputs.ir.command.tune_up_count"] = self._state.tune_up_count
            self._append_log("ir action: tune_up")
            return
        if action == _ACTION_TUNE_DOWN:
            self._state.tune_down_count += 1.0
            updates["inputs.ir.command.tune_down_count"] = self._state.tune_down_count
            self._append_log("ir action: tune_down")
            return
        if action == _ACTION_DASH_EXIT:
            self._state.dash_exit_count += 1.0
            updates["inputs.ir.command.dash_exit_count"] = self._state.dash_exit_count
            self._append_log("ir action: dash_exit")
            return
        if action == _ACTION_MENU_OPEN:
            self._state.menu_open_count += 1.0
            updates["inputs.ir.command.menu_open_count"] = self._state.menu_open_count
            self._append_log("ir action: menu_open")

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

    def _load_profile(self, profile_path: str) -> tuple[dict[str, str], dict[str, str]]:
        path = Path(profile_path)
        if not path.exists():
            return {}, {}
        try:
            raw = yaml.safe_load(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}, {}
        if not isinstance(raw, dict):
            return {}, {}

        remote_cfg = raw.get("remote", {})
        if not isinstance(remote_cfg, dict):
            return {}, {}
        mappings = remote_cfg.get("mappings", {})
        controls = remote_cfg.get("controls", {})
        scancodes = remote_cfg.get("scancodes", {})
        control_scancodes = remote_cfg.get("control_scancodes", {})

        parsed_keys: dict[str, str] = {}
        parsed_scans: dict[str, str] = {}
        if isinstance(mappings, dict):
            for key_name, action_name in mappings.items():
                if not isinstance(key_name, str) or not isinstance(action_name, str):
                    continue
                action_name_norm = self._normalize_action_name(action_name)
                if not action_name_norm:
                    continue
                scan_name_norm = self._normalize_scancode(key_name)
                if scan_name_norm:
                    parsed_scans[scan_name_norm] = action_name_norm
                    continue
                for key_alias in self._candidate_key_names(key_name):
                    parsed_keys[key_alias] = action_name_norm
        if isinstance(controls, dict):
            for action_name, key_name in controls.items():
                if not isinstance(action_name, str):
                    continue
                action_name_norm = self._normalize_action_name(action_name)
                if not action_name_norm:
                    continue
                if not isinstance(key_name, str) or not key_name.strip():
                    continue
                scan_name_norm = self._normalize_scancode(key_name)
                if scan_name_norm:
                    parsed_scans[scan_name_norm] = action_name_norm
                    continue
                for key_alias in self._candidate_key_names(key_name):
                    parsed_keys[key_alias] = action_name_norm
        if isinstance(scancodes, dict):
            for scan_name, action_name in scancodes.items():
                if not isinstance(scan_name, str) or not isinstance(action_name, str):
                    continue
                action_name_norm = self._normalize_action_name(action_name)
                scan_name_norm = self._normalize_scancode(scan_name)
                if not action_name_norm or not scan_name_norm:
                    continue
                parsed_scans[scan_name_norm] = action_name_norm
        if isinstance(control_scancodes, dict):
            for action_name, scan_name in control_scancodes.items():
                if not isinstance(action_name, str) or not isinstance(scan_name, str):
                    continue
                action_name_norm = self._normalize_action_name(action_name)
                scan_name_norm = self._normalize_scancode(scan_name)
                if not action_name_norm or not scan_name_norm:
                    continue
                parsed_scans[scan_name_norm] = action_name_norm
        return parsed_keys, parsed_scans

    def _candidate_key_names(self, keycode: Any) -> list[str]:
        names: list[str] = []
        if isinstance(keycode, (list, tuple, set)):
            raw_values = [str(item) for item in keycode]
        else:
            raw_values = [str(keycode)]

        seen: set[str] = set()
        for raw in raw_values:
            key = raw.strip().upper()
            if not key:
                continue
            candidates = [key]
            if key.startswith("KEY_"):
                candidates.append(key[4:])
            else:
                candidates.append(f"KEY_{key}")
            for candidate in candidates:
                if candidate and candidate not in seen:
                    seen.add(candidate)
                    names.append(candidate)
        return names

    def _resolve_action(self, key_names: list[str]) -> str:
        for key_name in key_names:
            action = self._key_action_map.get(key_name)
            if action:
                return action
        return ""

    def _should_emit_action(self, source_id: str, is_repeat: bool) -> bool:
        window_s = self._repeat_debounce_s if is_repeat else self._debounce_s
        if window_s <= 0.0:
            return True
        now_s = time.monotonic()
        last_s = self._last_emit_s_by_source.get(source_id, 0.0)
        if (now_s - last_s) < window_s:
            return False
        self._last_emit_s_by_source[source_id] = now_s
        return True

    def _normalize_scancode(self, value: Any) -> str:
        text = str(value).strip().lower()
        if not text:
            return ""
        if text.startswith("0x"):
            digits = text[2:]
            if not digits:
                return ""
            try:
                numeric = int(digits, 16)
            except ValueError:
                return ""
            return f"0x{numeric:x}"
        if text.isdigit():
            return f"0x{int(text):x}"
        return ""

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
            except OSError as exc:
                self._append_log(f"ir open failed path={device_path} error={exc}")
                return []

        if not device_name:
            self._append_log("ir device not configured: set device_path or device_name")
            return []
        try:
            from evdev import list_devices  # type: ignore
        except ImportError:
            return []
        matches: list[Any] = []
        name_exact = device_name.strip()
        name_lower = name_exact.lower()
        scanned: list[str] = []
        for candidate_path in list_devices():
            try:
                candidate = InputDevice(candidate_path)
            except OSError as exc:
                self._append_log(f"ir open failed candidate={candidate_path} error={exc}")
                continue
            candidate_name = str(candidate.name).strip()
            scanned.append(f"{candidate_path}:{candidate_name}")
            candidate_lower = candidate_name.lower()
            if candidate_name == name_exact or (name_lower and name_lower in candidate_lower):
                self._set_device_nonblocking(candidate)
                matches.append(candidate)
                continue
            try:
                candidate.close()
            except OSError:
                pass
        if not matches:
            sampled = ", ".join(scanned[:6]) if scanned else "<none>"
            self._append_log(
                f"ir device match not found for '{name_exact}'. scanned={len(scanned)} sample={sampled}"
            )
        return matches

    def _set_device_nonblocking(self, device: Any) -> None:
        """Handle evdev API differences across distro versions."""
        if hasattr(device, "set_blocking"):
            device.set_blocking(False)
            return
        fd = getattr(device, "fd", None)
        if isinstance(fd, int):
            os.set_blocking(fd, False)
