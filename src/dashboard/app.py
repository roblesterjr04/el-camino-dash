from __future__ import annotations

import json
from pathlib import Path
import signal
import time
from typing import Any
from typing import Callable

import pygame

from dashboard.config import DashboardConfig
from dashboard.config import ConfigError
from dashboard.config import load_dashboard_config
from dashboard.display.manager import DisplayManager
from dashboard.input.manager import InputManager
from dashboard.integrations.bluetooth_metadata import BluetoothMetadataAdapter
from dashboard.integrations.bluetooth_audio import BluetoothAudioAdapter
from dashboard.integrations.media_player import MediaPlayer
from dashboard.integrations.navit_adapter import NavitAdapter


class DashboardApp:
    def __init__(self, config: DashboardConfig) -> None:
        self._dashboard_config = config
        self.config_path = config.path
        self.config = config.raw
        self.navit = NavitAdapter(self.config["integrations"]["navit"])
        self.bluetooth = BluetoothMetadataAdapter(self.config["integrations"]["bluetooth_metadata"])
        self.bluetooth_audio = BluetoothAudioAdapter(self.config["integrations"].get("bluetooth_audio", {"enabled": False}))
        self.display_manager: DisplayManager | None = None
        self.input_manager: InputManager | None = None
        self.media: MediaPlayer | None = None
        self._values: dict[str, float] = {}
        self._running = False
        self._command_registry: dict[str, Callable[[dict[str, Any]], str | None]] = {}
        self._last_runtime_s = time.monotonic()
        self._odometer_path = Path(self.config["system"].get("odometer_state_path", "data/odometer_state.json"))
        self._odometer_persist_enabled = bool(self.config["system"].get("odometer_persist_enabled", True))
        self._odometer_write_interval_s = float(self.config["system"].get("odometer_write_interval_s", 2.0))
        self._last_odometer_write_s = time.monotonic()
        self._values["vehicle.odometer_miles"] = self._load_odometer_start_value()
        default_watch = str(self.config["system"].get("mode", "")).lower() == "simulation"
        self._config_watch_enabled = bool(self.config["system"].get("watch_config_file", default_watch))
        self._config_last_mtime_ns = self._read_config_mtime_ns()
        self._theme_watch_enabled = bool(self.config["system"].get("watch_theme_files", default_watch))
        self._theme_mtimes_ns = self._scan_theme_mtimes_ns()
        self._theme_persist_enabled = bool(self.config["system"].get("theme_persist_enabled", True))
        self._theme_state_path = Path(self.config["system"].get("theme_state_path", "data/theme_state.json"))
        self._load_persisted_theme()
        self._raw_input_log_enabled = bool(self.config["system"].get("raw_input_log_enabled", False))
        self._raw_input_log_interval_s = max(0.02, float(self.config["system"].get("raw_input_log_interval_s", 0.2)))
        self._raw_input_log_path = Path(self.config["system"].get("raw_input_log_path", "data/raw_inputs.log"))
        self._last_raw_input_log_s = time.monotonic()
        self._startup_sweep_started_s = time.monotonic()
        self._startup_sweep_complete = False
        self._previous_sigint_handler = None
        self._previous_sigterm_handler = None

    def run(self) -> None:
        self._install_signal_handlers()
        pygame.init()
        pygame.font.init()
        try:
            pygame.mouse.set_visible(False)
        except pygame.error:
            pass
        media_cfg = self.config["integrations"].get("media", {})
        if bool(media_cfg.get("enabled", False)):
            try:
                pygame.mixer.init()
            except pygame.error:
                pass
        self._reset_raw_input_log()
        self.input_manager = InputManager(self.config)
        self.media = MediaPlayer(media_cfg)
        command_registry = self._build_command_registry()
        self._command_registry = command_registry
        self.display_manager = DisplayManager(
            config=self.config,
            config_path=self.config_path,
            command_registry=command_registry,
            on_runtime_refresh=self._refresh_runtime_components,
        )
        self.navit.start()
        self.bluetooth_audio.start()
        self._running = True
        clock = pygame.time.Clock()

        try:
            while self._running:
                for event in pygame.event.get():
                    if self.display_manager and self.display_manager.handle_event(event):
                        self._running = False
                    if event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE:
                        self._running = False

                if self.input_manager and self.display_manager:
                    self.input_manager.set_keyboard_active(self.display_manager.keyboard_input_active())
                self._tick_inputs()
                self._log_raw_inputs()
                self._apply_gauge_smoothing()
                self._update_runtime_metrics()
                self.bluetooth_audio.poll()
                self._update_startup_sweep()
                self._reload_config_if_changed()
                self._reload_theme_if_changed()
                status = self._build_status()
                self.display_manager.render(values=self._values, status=status)
                fps = int(self.config["system"].get("target_fps", 30))
                clock.tick(fps)
        finally:
            self._persist_odometer(force=True)
            self._persist_theme()
            self.navit.stop()
            if self.display_manager:
                self.display_manager.shutdown()
            self._restore_signal_handlers()
            pygame.quit()

    def _install_signal_handlers(self) -> None:
        self._previous_sigint_handler = signal.getsignal(signal.SIGINT)
        self._previous_sigterm_handler = signal.getsignal(signal.SIGTERM)
        signal.signal(signal.SIGINT, self._handle_shutdown_signal)
        signal.signal(signal.SIGTERM, self._handle_shutdown_signal)

    def _restore_signal_handlers(self) -> None:
        if self._previous_sigint_handler is not None:
            signal.signal(signal.SIGINT, self._previous_sigint_handler)
        if self._previous_sigterm_handler is not None:
            signal.signal(signal.SIGTERM, self._previous_sigterm_handler)

    def _handle_shutdown_signal(self, signum: int, _frame: Any) -> None:
        print(f"Shutdown signal received ({signum}), exiting main loop")
        self._running = False

    def _tick_inputs(self) -> None:
        if not self.input_manager:
            return
        current = self.input_manager.poll()
        self._values.update(current)

    def _log_raw_inputs(self) -> None:
        if not self._raw_input_log_enabled:
            return
        now = time.monotonic()
        if (now - self._last_raw_input_log_s) < self._raw_input_log_interval_s:
            return
        self._last_raw_input_log_s = now

        tracked_prefixes = ("analog_inputs.", "speed_sensor.", "indicators.", "inputs.rotary.")
        snapshot: dict[str, float] = {}
        for key in sorted(self._values.keys()):
            if not key.startswith(tracked_prefixes):
                continue
            try:
                snapshot[key] = float(self._values[key])
            except (TypeError, ValueError):
                continue
        payload = {"ts": round(time.time(), 3), "values": snapshot}
        try:
            self._raw_input_log_path.parent.mkdir(parents=True, exist_ok=True)
            with self._raw_input_log_path.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(payload, sort_keys=True) + "\n")
        except OSError:
            return

    def _reset_raw_input_log(self) -> None:
        if not self._raw_input_log_enabled:
            return
        try:
            self._raw_input_log_path.parent.mkdir(parents=True, exist_ok=True)
            self._raw_input_log_path.write_text("", encoding="utf-8")
        except OSError:
            return

    def _apply_gauge_smoothing(self) -> None:
        for gauge_id, gauge in self.config["gauges"].items():
            src = gauge["source"]
            alpha = float(gauge["smoothing_alpha"])
            new_val = float(self._values.get(src, 0.0))
            smooth_key = f"gauges.{gauge_id}.value"
            prev_val = float(self._values.get(smooth_key, new_val))
            smoothed = (alpha * new_val) + ((1.0 - alpha) * prev_val)
            self._values[smooth_key] = smoothed

    def _build_status(self) -> dict[str, str]:
        theme_name = self.config["themes"]["active"]
        theme_root = Path(self.config["themes"]["theme_root"])
        theme_warning = ""
        if self.config["themes"]["allow_fallback"]:
            theme_file = theme_root / theme_name / "theme.json"
            if not theme_file.exists():
                theme_warning = "Theme missing: using fallback"

        return {
            "navit": self.navit.status(),
            "media": self.media.status() if self.media else "disabled",
            "bluetooth": self.bluetooth.status(),
            "bluetooth_audio": self.bluetooth_audio.status(),
            "theme_warning": theme_warning,
            "system_mode": str(self.config["system"].get("mode", "unknown")),
            "theme_active": str(self.config["themes"].get("active", "unknown")),
            "center_dial_mode": self._current_center_dial_mode(),
            "map_view_visible": "on" if bool(self.config.get("system", {}).get("map_view_visible", False)) else "off",
        }

    def _update_runtime_metrics(self) -> None:
        now = time.monotonic()
        dt = max(0.0, now - self._last_runtime_s)
        self._last_runtime_s = now
        speed_mph = float(self._values.get("speed_sensor.vehicle_speed", 0.0))
        miles_delta = speed_mph * (dt / 3600.0)
        self._values["vehicle.odometer_miles"] = float(self._values.get("vehicle.odometer_miles", 0.0)) + miles_delta
        self._persist_odometer(force=False)

    def _update_startup_sweep(self) -> None:
        if self._startup_sweep_complete:
            return
        sweep_enabled = bool(self.config["system"].get("startup_sweep_enabled", True))
        if not sweep_enabled:
            self._values.pop("system.startup_sweep_ratio", None)
            self._values.pop("system.indicators_startup_on", None)
            self._startup_sweep_complete = True
            return

        up_s = max(0.01, float(self.config["system"].get("startup_sweep_up_s", 1.1)))
        hold_s = max(0.0, float(self.config["system"].get("startup_sweep_hold_s", 0.12)))
        down_s = max(0.01, float(self.config["system"].get("startup_sweep_down_s", 0.7)))
        elapsed = max(0.0, time.monotonic() - self._startup_sweep_started_s)

        if elapsed <= up_s:
            ratio = elapsed / up_s
        elif elapsed <= up_s + hold_s:
            ratio = 1.0
        elif elapsed <= up_s + hold_s + down_s:
            ratio = 1.0 - ((elapsed - up_s - hold_s) / down_s)
        else:
            self._values.pop("system.startup_sweep_ratio", None)
            self._values.pop("system.indicators_startup_on", None)
            self._startup_sweep_complete = True
            return

        ratio = max(0.0, min(1.0, ratio))
        self._values["system.startup_sweep_ratio"] = ratio
        self._values["system.indicators_startup_on"] = 1.0
        self._apply_startup_sweep_sources(ratio)

    def _apply_startup_sweep_sources(self, ratio: float) -> None:
        # Sweep key analog/indicator signals so sub-gauges and indicator logic
        # visibly animate during startup, not only primary gauge needles.
        temp_min, temp_max = self._resolve_source_range("analog_inputs.coolant_temp", (100.0, 260.0))
        amp_min, amp_max = self._resolve_source_range("analog_inputs.system_amp", (-100.0, 100.0))
        oil_min, oil_max = self._resolve_source_range("indicators.oil", (0.0, 100.0))

        self._values["analog_inputs.coolant_temp"] = self._lerp(temp_min, temp_max, ratio)
        self._values["analog_inputs.system_amp"] = self._lerp(amp_min, amp_max, ratio)
        self._values["indicators.oil"] = self._lerp(oil_min, oil_max, ratio)

        # Mirror oil signal to pressure source for themes/configs that model oil
        # as a gauge input rather than indicator-only.
        if "analog_inputs.oil_pressure" in self._values or "oil" in self.config.get("gauges", {}):
            self._values["analog_inputs.oil_pressure"] = self._values["indicators.oil"]

    def _resolve_source_range(self, source: str, fallback: tuple[float, float]) -> tuple[float, float]:
        for spec in self.config.get("gauges", {}).values():
            if not isinstance(spec, dict):
                continue
            if str(spec.get("source", "")) != source:
                continue
            return float(spec.get("min_value", fallback[0])), float(spec.get("max_value", fallback[1]))

        sim = self.config.get("inputs", {}).get("simulated_data", {})
        if isinstance(sim, dict):
            range_map = {
                "analog_inputs.coolant_temp": "temp_range",
                "analog_inputs.system_amp": "amp_range",
                "analog_inputs.oil_pressure": "oil_range",
                "indicators.oil": "oil_range",
            }
            range_key = range_map.get(source, "")
            range_value = sim.get(range_key) if range_key else None
            if isinstance(range_value, list) and len(range_value) == 2:
                return float(range_value[0]), float(range_value[1])

        keyboard = self.config.get("inputs", {}).get("keyboard_sim", {})
        if isinstance(keyboard, dict) and source in {"analog_inputs.oil_pressure", "indicators.oil"}:
            return float(keyboard.get("oil_min", fallback[0])), float(keyboard.get("oil_max", fallback[1]))
        return fallback

    def _lerp(self, minimum: float, maximum: float, ratio: float) -> float:
        return minimum + ((maximum - minimum) * ratio)

    def _load_odometer_start_value(self) -> float:
        start_value = float(self.config["system"].get("odometer_start_miles", 0.0))
        if not self._odometer_persist_enabled:
            return start_value
        try:
            if self._odometer_path.exists():
                raw = json.loads(self._odometer_path.read_text(encoding="utf-8"))
                if isinstance(raw, dict) and "odometer_miles" in raw:
                    return float(raw["odometer_miles"])
        except (OSError, ValueError, json.JSONDecodeError):
            return start_value
        return start_value

    def _persist_odometer(self, force: bool) -> None:
        if not self._odometer_persist_enabled:
            return
        now = time.monotonic()
        if not force and (now - self._last_odometer_write_s) < self._odometer_write_interval_s:
            return
        self._last_odometer_write_s = now
        value = float(self._values.get("vehicle.odometer_miles", 0.0))
        payload = {"odometer_miles": value}
        try:
            self._odometer_path.parent.mkdir(parents=True, exist_ok=True)
            self._odometer_path.write_text(json.dumps(payload), encoding="utf-8")
        except OSError:
            return

    def _build_command_registry(self) -> dict[str, Callable[[dict[str, Any]], str | None]]:
        return {
            "navit.start": self._cmd_navit_start,
            "navit.stop": self._cmd_navit_stop,
            "media.play_pause": self._cmd_media_play_pause,
            "media.next": self._cmd_media_next,
            "media.stop": self._cmd_media_stop,
            "media.set_volume": self._cmd_media_set_volume,
            "bluetooth_audio.reconnect": self._cmd_bluetooth_audio_reconnect,
            "theme.next": self._cmd_theme_next,
            "center_dial_mode.toggle": self._cmd_center_dial_mode_toggle,
            "map_view.toggle": self._cmd_map_view_toggle,
            "app.restart": self._cmd_app_restart,
        }

    def _refresh_runtime_components(self) -> None:
        default_watch = str(self.config["system"].get("mode", "")).lower() == "simulation"
        self._config_watch_enabled = bool(self.config["system"].get("watch_config_file", default_watch))
        self._config_last_mtime_ns = self._read_config_mtime_ns()
        self._theme_watch_enabled = bool(self.config["system"].get("watch_theme_files", default_watch))
        self._theme_mtimes_ns = self._scan_theme_mtimes_ns()
        self._theme_persist_enabled = bool(self.config["system"].get("theme_persist_enabled", True))
        self._theme_state_path = Path(self.config["system"].get("theme_state_path", "data/theme_state.json"))
        self._load_persisted_theme()
        self._raw_input_log_enabled = bool(self.config["system"].get("raw_input_log_enabled", False))
        self._raw_input_log_interval_s = max(0.02, float(self.config["system"].get("raw_input_log_interval_s", 0.2)))
        self._raw_input_log_path = Path(self.config["system"].get("raw_input_log_path", "data/raw_inputs.log"))
        self._last_raw_input_log_s = time.monotonic()
        self._odometer_path = Path(self.config["system"].get("odometer_state_path", "data/odometer_state.json"))
        self._odometer_persist_enabled = bool(self.config["system"].get("odometer_persist_enabled", True))
        self._odometer_write_interval_s = float(self.config["system"].get("odometer_write_interval_s", 2.0))
        self.input_manager = InputManager(self.config)
        self.media = MediaPlayer(self.config["integrations"]["media"])
        if self.config["integrations"]["navit"].get("enabled", False):
            self.navit.start()
        else:
            self.navit.stop()
        self.bluetooth_audio = BluetoothAudioAdapter(self.config["integrations"].get("bluetooth_audio", {"enabled": False}))
        self.bluetooth_audio.start()
        if self.display_manager and self._command_registry:
            self.display_manager.shutdown()
            self.display_manager = DisplayManager(
                config=self.config,
                config_path=self.config_path,
                command_registry=self._command_registry,
                on_runtime_refresh=self._refresh_runtime_components,
            )

    def _read_config_mtime_ns(self) -> int:
        try:
            return self.config_path.stat().st_mtime_ns
        except OSError:
            return 0

    def _reload_config_if_changed(self) -> None:
        if not self._config_watch_enabled:
            return
        current_mtime = self._read_config_mtime_ns()
        if current_mtime <= 0 or current_mtime == self._config_last_mtime_ns:
            return
        self._config_last_mtime_ns = current_mtime
        previous_odo = float(self._values.get("vehicle.odometer_miles", 0.0))
        try:
            loaded = load_dashboard_config(self.config_path)
        except (ConfigError, OSError):
            return
        self.config.clear()
        self.config.update(loaded.raw)
        self._persist_theme()
        self._values["vehicle.odometer_miles"] = previous_odo
        self._refresh_runtime_components()

    def _scan_theme_mtimes_ns(self) -> dict[str, int]:
        theme_root = Path(self.config["themes"].get("theme_root", "themes"))
        if not theme_root.exists():
            return {}
        mtimes: dict[str, int] = {}
        for theme_json in theme_root.glob("*/theme.json"):
            try:
                mtimes[str(theme_json)] = theme_json.stat().st_mtime_ns
            except OSError:
                continue
        return mtimes

    def _reload_theme_if_changed(self) -> None:
        if not self._theme_watch_enabled:
            return
        current = self._scan_theme_mtimes_ns()
        if current == self._theme_mtimes_ns:
            return
        self._theme_mtimes_ns = current
        self._refresh_runtime_components()

    def _cmd_navit_start(self, _: dict[str, Any]) -> str:
        self.navit.start()
        return "Navit start requested"

    def _cmd_navit_stop(self, _: dict[str, Any]) -> str:
        self.navit.stop()
        return "Navit stop requested"

    def _cmd_media_play_pause(self, _: dict[str, Any]) -> str:
        if not self.media:
            return "Media unavailable"
        self.media.toggle_play_pause()
        return "Media toggled"

    def _cmd_media_next(self, _: dict[str, Any]) -> str:
        if not self.media:
            return "Media unavailable"
        self.media.next_track()
        return "Next track"

    def _cmd_media_stop(self, _: dict[str, Any]) -> str:
        if not self.media:
            return "Media unavailable"
        self.media.stop()
        return "Media stopped"

    def _cmd_media_set_volume(self, payload: dict[str, Any]) -> str:
        if not self.media:
            return "Media unavailable"
        volume = float(payload.get("value", self.config["integrations"]["media"].get("startup_volume", 0.5)))
        self.media.set_volume(volume)
        return f"Volume {volume:.2f}"

    def _cmd_bluetooth_audio_reconnect(self, _: dict[str, Any]) -> str:
        self.bluetooth_audio.reconnect()
        return f"Bluetooth audio: {self.bluetooth_audio.status()}"

    def _cmd_app_restart(self, _: dict[str, Any]) -> str:
        self._running = False
        return "Stopping app (restart requested)"

    def _cmd_theme_next(self, _: dict[str, Any]) -> str:
        theme_root = Path(self.config["themes"]["theme_root"])
        if not theme_root.exists():
            return "Theme root missing"
        available = sorted(
            path.name for path in theme_root.iterdir() if path.is_dir() and (path / "theme.json").exists()
        )
        if not available:
            return "No themes found"
        active = str(self.config["themes"].get("active", ""))
        if active in available:
            next_index = (available.index(active) + 1) % len(available)
        else:
            next_index = 0
        self.config["themes"]["active"] = available[next_index]
        self._persist_theme()
        self._refresh_runtime_components()
        return f"Theme: {available[next_index]}"

    def _cmd_center_dial_mode_toggle(self, _: dict[str, Any]) -> str:
        theme_root = Path(self.config["themes"].get("theme_root", "themes"))
        theme_name = str(self.config["themes"].get("active", "")).strip()
        if not theme_name:
            return "No active theme"
        theme_path = theme_root / theme_name / "theme.json"
        if not theme_path.exists():
            return "Theme file missing"
        try:
            raw = json.loads(theme_path.read_text(encoding="utf-8"))
        except (OSError, ValueError, json.JSONDecodeError):
            return "Theme load failed"
        if not isinstance(raw, dict):
            return "Theme format invalid"
        mode_cfg = raw.get("center_dial_mode", {})
        current_mode = self._center_mode_from_theme_data(raw)
        next_mode = "clock" if current_mode == "tach" else "tach"
        if isinstance(mode_cfg, str):
            raw["center_dial_mode"] = {"_default": mode_cfg, "gauge_center": next_mode}
        elif isinstance(mode_cfg, dict):
            updated = dict(mode_cfg)
            updated["gauge_center"] = next_mode
            raw["center_dial_mode"] = updated
        else:
            raw["center_dial_mode"] = {"_default": "tach", "gauge_center": next_mode}
        try:
            theme_path.write_text(json.dumps(raw, indent=2), encoding="utf-8")
        except OSError:
            return "Theme save failed"
        self._refresh_runtime_components()
        return f"Center dial: {next_mode}"

    def _cmd_map_view_toggle(self, _: dict[str, Any]) -> str:
        current = bool(self.config.get("system", {}).get("map_view_visible", False))
        next_visible = not current
        self.config.setdefault("system", {})["map_view_visible"] = next_visible
        self._refresh_runtime_components()
        return f"Map view: {'on' if next_visible else 'off'}"

    def _center_mode_from_theme_data(self, theme_data: dict[str, Any]) -> str:
        mode_cfg = theme_data.get("center_dial_mode", "tach")
        if isinstance(mode_cfg, str):
            mode = mode_cfg
        elif isinstance(mode_cfg, dict):
            display_mode = mode_cfg.get("gauge_center")
            if isinstance(display_mode, str):
                mode = display_mode
            else:
                default_mode = mode_cfg.get("_default", "tach")
                mode = default_mode if isinstance(default_mode, str) else "tach"
        else:
            mode = "tach"
        mode = mode.strip().lower()
        return "clock" if mode == "clock" else "tach"

    def _current_center_dial_mode(self) -> str:
        theme_root = Path(self.config["themes"].get("theme_root", "themes"))
        theme_name = str(self.config["themes"].get("active", "")).strip()
        if not theme_name:
            return "tach"
        theme_path = theme_root / theme_name / "theme.json"
        if not theme_path.exists():
            return "tach"
        try:
            raw = json.loads(theme_path.read_text(encoding="utf-8"))
        except (OSError, ValueError, json.JSONDecodeError):
            return "tach"
        if not isinstance(raw, dict):
            return "tach"
        return self._center_mode_from_theme_data(raw)

    def _load_persisted_theme(self) -> None:
        if not self._theme_persist_enabled:
            return
        try:
            if not self._theme_state_path.exists():
                return
            raw = json.loads(self._theme_state_path.read_text(encoding="utf-8"))
            if not isinstance(raw, dict):
                return
            theme_name = str(raw.get("active_theme", "")).strip()
            if not theme_name:
                return
            theme_root = Path(self.config["themes"].get("theme_root", "themes"))
            theme_file = theme_root / theme_name / "theme.json"
            if not theme_file.exists():
                return
            self.config["themes"]["active"] = theme_name
        except (OSError, ValueError, json.JSONDecodeError):
            return

    def _persist_theme(self) -> None:
        if not self._theme_persist_enabled:
            return
        active = str(self.config.get("themes", {}).get("active", "")).strip()
        if not active:
            return
        payload = {"active_theme": active}
        try:
            self._theme_state_path.parent.mkdir(parents=True, exist_ok=True)
            self._theme_state_path.write_text(json.dumps(payload), encoding="utf-8")
        except OSError:
            return
