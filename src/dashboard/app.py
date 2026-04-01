from __future__ import annotations

import json
import math
from datetime import datetime
from pathlib import Path
import signal
import threading
import time
from typing import Any
from typing import Callable

import pygame

from dashboard.config import DashboardConfig
from dashboard.config import ConfigError
from dashboard.config import load_dashboard_config
from dashboard.display.manager import DisplayManager
from dashboard.input.manager import InputManager
from dashboard.input.stable_readings import SensorReadingStability
from dashboard.integrations.audio_router import AudioRouterAdapter
from dashboard.integrations.bluetooth_metadata import BluetoothMetadataAdapter
from dashboard.integrations.bluetooth_audio import BluetoothAudioAdapter
from dashboard.integrations.fm_radio import FmRadioAdapter
from dashboard.integrations.media_player import MediaPlayer
from dashboard.integrations.navit_adapter import NavitAdapter
from dashboard.integrations.nano_bridge import NanoBridge
from dashboard.integrations.esp32_audio_link import Esp32AudioLink
from dashboard.integrations.attiny_power_link import AttinyPowerLink
from dashboard.integrations.wifi_nm import wifi_connect
from dashboard.integrations.wifi_nm import wifi_disconnect
from dashboard.integrations.wifi_nm import wifi_radio_toggle
from dashboard.integrations.wifi_nm import wifi_rescan
from dashboard.integrations.wifi_nm import wifi_status_dict
from dashboard.theme_loader import load_theme_data

try:
    from gpiozero import DigitalInputDevice  # type: ignore
except ImportError:  # pragma: no cover
    DigitalInputDevice = None


ARDUINO_FAULT_LABELS: list[tuple[int, str]] = [
    (1 << 0, "POT OPEN"),
    (1 << 1, "POT SHORT"),
    (1 << 2, "TEMP OPEN"),
    (1 << 3, "TEMP SHORT"),
    (1 << 4, "PRESS OPEN"),
    (1 << 5, "PRESS SHORT"),
    (1 << 6, "VOLT OPEN"),
    (1 << 7, "VOLT HIGH"),
    (1 << 8, "HALL MISSING"),
    (1 << 9, "COIL MISSING"),
    (1 << 10, "ALT STUCK"),
    (1 << 11, "POT POWER"),
    (1 << 12, "TEMP POWER"),
    (1 << 13, "PRESS POWER"),
    (1 << 14, "VOLT POWER"),
]


class DashboardApp:
    def __init__(self, config: DashboardConfig) -> None:
        self._dashboard_config = config
        self.config_path = config.path
        self.config = config.raw
        self.navit = NavitAdapter(self.config["integrations"]["navit"])
        self.bluetooth = BluetoothMetadataAdapter(self.config["integrations"]["bluetooth_metadata"])
        self.bluetooth_audio = BluetoothAudioAdapter(self.config["integrations"].get("bluetooth_audio", {"enabled": False}))
        self.audio_router = AudioRouterAdapter(self.config["integrations"].get("audio_router", {"enabled": False}))
        self.nano_bridge = NanoBridge(self.config["integrations"].get("nano_bridge", {"enabled": False}))
        self.esp32_link = Esp32AudioLink(self.config["integrations"].get("esp32_audio_link", {"enabled": False}))
        self.attiny_power = AttinyPowerLink(self.config["integrations"].get("attiny_power", {"enabled": False}))
        self.display_manager: DisplayManager | None = None
        self.input_manager: InputManager | None = None
        self.media: MediaPlayer | None = None
        self.radio: FmRadioAdapter | None = None
        self._values: dict[str, float] = {}
        self._sensor_stability = SensorReadingStability(self.config["inputs"].get("sensor_reading_stability", {"enabled": False}))
        self._oled_ambient_display_int: int | None = None
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
        self._fault_latched_word = 0
        self._fault_last_word = 0
        self._previous_sigint_handler = None
        self._previous_sigterm_handler = None
        self._ir_last_counts: dict[str, int] = {
            "volume_up": 0,
            "volume_down": 0,
            "theme_next": 0,
            "map_toggle": 0,
            "center_toggle": 0,
            "tune_up": 0,
            "tune_down": 0,
            "dash_exit": 0,
            "menu_open": 0,
            "diagnostics_open": 0,
        }
        self._ir_last_code = "-"
        self._last_aux_press_count = 0
        self._integration_start_thread: threading.Thread | None = None
        self._last_encoder_status_log_s = 0.0
        self._diagnostic_pin = None
        self._diagnostic_pin_state = "-"

    def _available_theme_names(self) -> list[str]:
        theme_root = Path(self.config["themes"]["theme_root"])
        if not theme_root.exists():
            return []
        discovered = sorted(
            path.name for path in theme_root.iterdir() if path.is_dir() and (path / "theme.json").exists()
        )
        enabled = self.config.get("themes", {}).get("enabled")
        if not isinstance(enabled, list):
            return discovered
        allowlist = {str(name).strip() for name in enabled if str(name).strip()}
        filtered = [name for name in discovered if name in allowlist]
        return filtered if filtered else discovered

    def _refresh_theme_components(self) -> None:
        if not self.display_manager:
            return
        refreshed = self.display_manager.refresh_theme()
        if refreshed:
            self._persist_theme()
            self._theme_mtimes_ns = self._scan_theme_mtimes_ns()
            return
        # Theme changed display topology; rebuild display layer only.
        self.display_manager.shutdown()
        self.display_manager = DisplayManager(
            config=self.config,
            config_path=self.config_path,
            command_registry=self._command_registry,
            on_runtime_refresh=self._refresh_runtime_components,
        )
        self._persist_theme()
        self._theme_mtimes_ns = self._scan_theme_mtimes_ns()

    def run(self) -> None:
        startup_t0 = time.monotonic()
        print("[startup] run begin", flush=True)
        self._install_signal_handlers()
        # Avoid global pygame.init() because it can initialize mixer/audio
        # backends that are unstable on some Pi Pulse/ALSA stacks.
        pygame.display.init()
        print(f"[startup] pygame.display.init at {time.monotonic() - startup_t0:.3f}s", flush=True)
        pygame.font.init()
        print(f"[startup] pygame.font.init at {time.monotonic() - startup_t0:.3f}s", flush=True)
        try:
            pygame.mouse.set_visible(False)
        except pygame.error:
            pass
        media_cfg = self.config["integrations"].get("media", {})
        bt_audio_cfg = self.config["integrations"].get("bluetooth_audio", {})
        bt_audio_enabled = isinstance(bt_audio_cfg, dict) and bool(bt_audio_cfg.get("enabled", False))
        use_local_mixer = bool(media_cfg.get("enabled", False)) and (not bt_audio_enabled)
        if use_local_mixer:
            try:
                pygame.mixer.init()
            except pygame.error:
                pass
        self._reset_raw_input_log()
        command_registry = self._build_command_registry()
        self._command_registry = command_registry
        print(f"[startup] building display manager at {time.monotonic() - startup_t0:.3f}s", flush=True)
        self.display_manager = DisplayManager(
            config=self.config,
            config_path=self.config_path,
            command_registry=command_registry,
            on_runtime_refresh=self._refresh_runtime_components,
        )
        print(f"[startup] display manager ready at {time.monotonic() - startup_t0:.3f}s", flush=True)
        # Force an immediate first frame so the startup placeholder remains visible
        # while slower runtime integrations initialize.
        self.display_manager.render(values=self._values, status=self._build_status())
        print(f"[startup] first frame submitted at {time.monotonic() - startup_t0:.3f}s", flush=True)
        self.input_manager = InputManager(self.config)
        self.nano_bridge.bind_serial_writer(self.input_manager.nano_serial_writer())
        self.media = MediaPlayer(media_cfg)
        self.radio = FmRadioAdapter(self.config["integrations"].get("fm_radio", {"enabled": False}))
        self._start_integrations_async(startup_t0)
        self._init_diagnostic_pin()
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
                self._update_alert_states()
                self._update_fault_registry()
                self.bluetooth.poll()
                self.bluetooth_audio.poll()
                if self.radio:
                    self.radio.tick()
                self._tick_power_links()
                self._update_startup_sweep()
                self._reload_config_if_changed()
                self._reload_theme_if_changed()
                status = self._build_status()
                self._publish_oled_and_audio(status)
                self.display_manager.render(values=self._values, status=status)
                fps = int(self.config["system"].get("target_fps", 30))
                clock.tick(fps)
        finally:
            self._persist_odometer(force=True)
            self._persist_theme()
            self.navit.stop()
            if self.radio:
                self.radio.stop()
            self.audio_router.stop()
            self.nano_bridge.stop()
            self.esp32_link.stop()
            self.attiny_power.stop()
            if self.display_manager:
                self.display_manager.shutdown()
            self._restore_signal_handlers()
            pygame.quit()

    def _install_signal_handlers(self) -> None:
        self._previous_sigint_handler = signal.getsignal(signal.SIGINT)
        self._previous_sigterm_handler = signal.getsignal(signal.SIGTERM)
        signal.signal(signal.SIGINT, self._handle_shutdown_signal)
        signal.signal(signal.SIGTERM, self._handle_shutdown_signal)

    def _start_integrations_async(self, startup_t0: float) -> None:
        if self._integration_start_thread and self._integration_start_thread.is_alive():
            return

        def _starter() -> None:
            try:
                if self.radio:
                    self.radio.start()
                print(f"[startup] radio started at {time.monotonic() - startup_t0:.3f}s", flush=True)
            except Exception as exc:
                print(f"[startup] radio start failed: {exc}", flush=True)
            try:
                self.audio_router.start()
                print(f"[startup] audio router started at {time.monotonic() - startup_t0:.3f}s", flush=True)
            except Exception as exc:
                print(f"[startup] audio router start failed: {exc}", flush=True)
            try:
                self.navit.start()
                print(f"[startup] navit started at {time.monotonic() - startup_t0:.3f}s", flush=True)
            except Exception as exc:
                print(f"[startup] navit start failed: {exc}", flush=True)
            try:
                self.bluetooth.start()
                print(f"[startup] bluetooth metadata started at {time.monotonic() - startup_t0:.3f}s", flush=True)
            except Exception as exc:
                print(f"[startup] bluetooth metadata start failed: {exc}", flush=True)
            try:
                self.bluetooth_audio.start()
                print(f"[startup] bluetooth audio started at {time.monotonic() - startup_t0:.3f}s", flush=True)
            except Exception as exc:
                print(f"[startup] bluetooth audio start failed: {exc}", flush=True)
            try:
                self.nano_bridge.start()
                self.esp32_link.start()
                self.attiny_power.start()
            except Exception as exc:
                print(f"[startup] hardware bridge start failed: {exc}", flush=True)

        self._integration_start_thread = threading.Thread(target=_starter, name="dash-integrations-start", daemon=True)
        self._integration_start_thread.start()

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
        aux_presses = int(current.get("inputs.rotary.aux.presses", 0.0))
        if aux_presses < self._last_aux_press_count:
            self._last_aux_press_count = aux_presses
        aux_press_delta = max(0, aux_presses - self._last_aux_press_count)
        if aux_press_delta > 0:
            # Route AUX press through the same edge-triggered diagnostics
            # counter consumed by the menu overlay controller.
            diag_count = int(
                current.get(
                    "inputs.ir.command.diagnostics_open_count",
                    self._values.get("inputs.ir.command.diagnostics_open_count", 0.0),
                )
            )
            current["inputs.ir.command.diagnostics_open_count"] = float(diag_count + aux_press_delta)
        self._last_aux_press_count = aux_presses
        # Stabilize sensor keys here (not in InputManager) so the same dict becomes _values and feeds
        # HDMI gauges, raw_input log, and NanoBridge OLED WT= token (encoder display downlink).
        current = self._sensor_stability.apply(current)
        self._values.update(current)
        self._ir_last_code = self.input_manager.ir_last_code() or "-"
        self._apply_ir_commands(current)

    def _apply_ir_commands(self, current: dict[str, float]) -> None:
        command_map = {
            "theme_next": ("inputs.ir.command.theme_next_count", "theme.next"),
            "map_toggle": ("inputs.ir.command.map_toggle_count", "map_view.toggle"),
            "center_toggle": ("inputs.ir.command.center_toggle_count", "center_dial_mode.toggle"),
            "tune_up": ("inputs.ir.command.tune_up_count", "radio.tune_up"),
            "tune_down": ("inputs.ir.command.tune_down_count", "radio.tune_down"),
            "dash_exit": ("inputs.ir.command.dash_exit_count", "app.main_screen"),
            "menu_open": ("inputs.ir.command.menu_open_count", "menu.open_back"),
        }
        for state_key, (value_key, command_id) in command_map.items():
            count = int(current.get(value_key, 0.0))
            last = int(self._ir_last_counts.get(state_key, 0))
            if count < last:
                self._ir_last_counts[state_key] = count
                continue
            repeats = count - last
            if repeats > 0:
                handler = self._command_registry.get(command_id)
                if handler is not None:
                    for _ in range(repeats):
                        handler({})
            self._ir_last_counts[state_key] = count

        volume_up = int(current.get("inputs.ir.command.volume_up_count", 0.0))
        volume_down = int(current.get("inputs.ir.command.volume_down_count", 0.0))
        self._apply_ir_volume_steps(up_steps=volume_up, down_steps=volume_down)
        diagnostics_open = int(current.get("inputs.ir.command.diagnostics_open_count", 0.0))
        self._ir_last_counts["diagnostics_open"] = max(0, diagnostics_open)

    def _apply_ir_volume_steps(self, up_steps: int, down_steps: int) -> None:
        prev_up = int(self._ir_last_counts.get("volume_up", 0))
        prev_down = int(self._ir_last_counts.get("volume_down", 0))
        if up_steps < prev_up:
            prev_up = up_steps
        if down_steps < prev_down:
            prev_down = down_steps
        delta_steps = (up_steps - prev_up) - (down_steps - prev_down)
        self._ir_last_counts["volume_up"] = up_steps
        self._ir_last_counts["volume_down"] = down_steps
        if delta_steps == 0:
            return
        media_cfg = self.config.get("integrations", {}).get("media", {})
        if not isinstance(media_cfg, dict):
            return
        current_volume = float(media_cfg.get("startup_volume", 0.5))
        next_volume = max(0.0, min(1.0, current_volume + (0.05 * float(delta_steps))))
        media_cfg["startup_volume"] = next_volume
        if self.media:
            self.media.set_volume(next_volume)

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

        active_word = int(self._values.get("arduino.fault_word", 0.0))
        latched_word = int(self._values.get("arduino.fault_latched_word", 0.0))
        active_faults = self._fault_labels_for_word(active_word)
        latched_faults = self._fault_labels_for_word(latched_word)
        lines = (active_faults or latched_faults)[:3]
        while len(lines) < 3:
            lines.append("-")
        bt_audio_cfg = self.config.get("integrations", {}).get("bluetooth_audio", {})
        bt_audio_enabled = isinstance(bt_audio_cfg, dict) and bool(bt_audio_cfg.get("enabled", False))
        media_status = self.media.status() if self.media else "disabled"
        media_track = "-"
        if bt_audio_enabled:
            media_status = self.bluetooth_audio.media_status()
            media_track = self.bluetooth_audio.media_track()
        encoder_last_frame = "-"
        if self.input_manager:
            encoder_last_frame = self.input_manager.encoder_last_frame() or "-"
        now_s = time.monotonic()
        if (now_s - self._last_encoder_status_log_s) >= 2.0:
            print(f"[encoder-status] {encoder_last_frame}", flush=True)
            self._last_encoder_status_log_s = now_s

        base_status = {
            "navit": self.navit.status(),
            "media": media_status,
            "media_track": media_track,
            "bluetooth": self.bluetooth.status(),
            "bluetooth_audio": self.bluetooth_audio.status(),
            "fm_radio": self.radio.status() if self.radio else "disabled",
            "audio_source": self.audio_router.active_source(),
            "audio_router": self.audio_router.status(),
            "theme_warning": theme_warning,
            "system_mode": str(self.config["system"].get("mode", "unknown")),
            "theme_active": str(self.config["themes"].get("active", "unknown")),
            "center_dial_mode": self._current_center_dial_mode(),
            "map_view_visible": "on" if bool(self.config.get("system", {}).get("map_view_visible", False)) else "off",
            "ir_last_code": self._ir_last_code,
            "encoder_last_frame": encoder_last_frame,
            "ir_diag_state": f"open x{int(self._ir_last_counts.get('diagnostics_open', 0))}",
            "fault_status": "ACTIVE" if active_faults else ("LATCHED" if latched_faults else "CLEAR"),
            "fault_active_count": str(len(active_faults)),
            "fault_latched_count": str(len(latched_faults)),
            "fault_code_1": lines[0],
            "fault_code_2": lines[1],
            "fault_code_3": lines[2],
            "fault_headline": lines[0] if lines[0] != "-" else "Fault detected",
            "diagnostics_pin_state": self._diagnostic_pin_state,
            "nano_bridge": self.nano_bridge.status(),
            "esp32_link": self.esp32_link.status(),
            "sensor_pack": self.input_manager.sensor_pack_source() if self.input_manager else "off",
            "attiny_power": self.attiny_power.status(),
        }
        base_status.update(wifi_status_dict(self.config))
        return base_status

    def _update_runtime_metrics(self) -> None:
        now = time.monotonic()
        dt = max(0.0, now - self._last_runtime_s)
        self._last_runtime_s = now
        speed_mph = float(self._values.get("speed_sensor.vehicle_speed", 0.0))
        miles_delta = speed_mph * (dt / 3600.0)
        self._values["vehicle.odometer_miles"] = float(self._values.get("vehicle.odometer_miles", 0.0)) + miles_delta
        self._persist_odometer(force=False)

    def _update_alert_states(self) -> None:
        system_cfg = self.config.get("system", {})
        enabled = bool(system_cfg.get("low_oil_pressure_alert_enabled", True))
        source = str(system_cfg.get("low_oil_pressure_source", "analog_inputs.oil_pressure")).strip()
        threshold = float(system_cfg.get("low_oil_pressure_threshold", 20.0))
        if (not enabled) or (not source):
            self._values["alerts.low_oil_pressure"] = 0.0
            return
        pressure = float(self._values.get(source, 0.0))
        self._values["alerts.low_oil_pressure"] = 1.0 if pressure < threshold else 0.0

    def _update_fault_registry(self) -> None:
        active_word = int(self._values.get("arduino.fault_word", 0.0))
        self._fault_last_word = active_word
        if active_word != 0:
            self._fault_latched_word |= active_word
        self._values["arduino.fault_active_word"] = float(active_word)
        self._values["arduino.fault_latched_word"] = float(self._fault_latched_word)
        self._values["arduino.fault_active_count"] = float(len(self._fault_labels_for_word(active_word)))
        self._values["arduino.fault_latched_count"] = float(len(self._fault_labels_for_word(self._fault_latched_word)))

    def _fault_labels_for_word(self, word: int) -> list[str]:
        labels = [label for bit, label in ARDUINO_FAULT_LABELS if word & bit]
        unknown_bits = word & (~sum(bit for bit, _ in ARDUINO_FAULT_LABELS))
        if unknown_bits:
            labels.append(f"UNKNOWN 0x{unknown_bits:X}")
        return labels

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
        amb_min, amb_max = self._resolve_source_range("analog_inputs.ambient_temp", (45.0, 85.0))
        self._values["analog_inputs.ambient_temp"] = self._lerp(amb_min, amb_max, ratio)
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
                "analog_inputs.ambient_temp": "ambient_range",
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
            "radio.fm_mode": self._cmd_radio_fm_mode,
            "radio.wb_mode": self._cmd_radio_wb_mode,
            "radio.seek_up": self._cmd_radio_seek_up,
            "radio.seek_down": self._cmd_radio_seek_down,
            "radio.tune_up": self._cmd_radio_tune_up,
            "radio.tune_down": self._cmd_radio_tune_down,
            "radio.scan_cycle": self._cmd_radio_scan_cycle,
            "radio.set_volume": self._cmd_radio_set_volume,
            "radio.mute_toggle": self._cmd_radio_mute_toggle,
            "audio.source_pi": self._cmd_audio_source_pi,
            "audio.source_fm": self._cmd_audio_source_fm,
            "audio.select_source": self._cmd_audio_select_source,
            "bluetooth_audio.reconnect": self._cmd_bluetooth_audio_reconnect,
            "bluetooth.connect": self._cmd_bluetooth_connect,
            "bluetooth.disconnect": self._cmd_bluetooth_disconnect,
            "theme.next": self._cmd_theme_next,
            "center_dial_mode.toggle": self._cmd_center_dial_mode_toggle,
            "map_view.toggle": self._cmd_map_view_toggle,
            "app.main_screen": self._cmd_app_main_screen,
            "menu.open_back": self._cmd_menu_open_back,
            "menu.close": self._cmd_menu_close,
            "faults.clear": self._cmd_faults_clear,
            "app.restart": self._cmd_app_restart,
            "wifi.rescan": self._cmd_wifi_rescan,
            "wifi.connect": self._cmd_wifi_connect,
            "wifi.disconnect": self._cmd_wifi_disconnect,
            "wifi.radio_toggle": self._cmd_wifi_radio_toggle,
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
        self._sensor_stability = SensorReadingStability(self.config["inputs"].get("sensor_reading_stability", {"enabled": False}))
        self.media = MediaPlayer(self.config["integrations"]["media"])
        if self.config["integrations"]["navit"].get("enabled", False):
            self.navit.start()
        else:
            self.navit.stop()
        self.bluetooth = BluetoothMetadataAdapter(self.config["integrations"]["bluetooth_metadata"])
        self.bluetooth.start()
        self.bluetooth_audio = BluetoothAudioAdapter(self.config["integrations"].get("bluetooth_audio", {"enabled": False}))
        self.bluetooth_audio.start()
        if self.radio:
            self.radio.stop()
        self.radio = FmRadioAdapter(self.config["integrations"].get("fm_radio", {"enabled": False}))
        self.radio.start()
        self.audio_router.stop()
        self.audio_router = AudioRouterAdapter(self.config["integrations"].get("audio_router", {"enabled": False}))
        self.audio_router.start()
        self.nano_bridge.stop()
        self.esp32_link.stop()
        self.attiny_power.stop()
        self.nano_bridge = NanoBridge(self.config["integrations"].get("nano_bridge", {"enabled": False}))
        self.nano_bridge.bind_serial_writer(self.input_manager.nano_serial_writer())
        self.esp32_link = Esp32AudioLink(self.config["integrations"].get("esp32_audio_link", {"enabled": False}))
        self.attiny_power = AttinyPowerLink(self.config["integrations"].get("attiny_power", {"enabled": False}))
        self.nano_bridge.start()
        self.esp32_link.start()
        self.attiny_power.start()
        self._init_diagnostic_pin()
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
        self._refresh_theme_components()

    def _cmd_navit_start(self, _: dict[str, Any]) -> str:
        self.navit.start()
        return "Navit start requested"

    def _cmd_navit_stop(self, _: dict[str, Any]) -> str:
        self.navit.stop()
        return "Navit stop requested"

    def _cmd_media_play_pause(self, _: dict[str, Any]) -> str:
        self.audio_router.select_source("pi_dac")
        bt_cfg = self.config.get("integrations", {}).get("bluetooth_audio", {})
        if isinstance(bt_cfg, dict) and bool(bt_cfg.get("enabled", False)):
            return f"Bluetooth audio: {self.bluetooth_audio.media_play_pause()}"
        if not self.media:
            return "Media unavailable"
        self.media.toggle_play_pause()
        return "Media toggled"

    def _cmd_media_next(self, _: dict[str, Any]) -> str:
        self.audio_router.select_source("pi_dac")
        bt_cfg = self.config.get("integrations", {}).get("bluetooth_audio", {})
        if isinstance(bt_cfg, dict) and bool(bt_cfg.get("enabled", False)):
            return f"Bluetooth audio: {self.bluetooth_audio.media_next()}"
        if not self.media:
            return "Media unavailable"
        self.media.next_track()
        return "Next track"

    def _cmd_media_stop(self, _: dict[str, Any]) -> str:
        self.audio_router.select_source("pi_dac")
        bt_cfg = self.config.get("integrations", {}).get("bluetooth_audio", {})
        if isinstance(bt_cfg, dict) and bool(bt_cfg.get("enabled", False)):
            return f"Bluetooth audio: {self.bluetooth_audio.media_stop()}"
        if not self.media:
            return "Media unavailable"
        self.media.stop()
        return "Media stopped"

    def _cmd_media_set_volume(self, payload: dict[str, Any]) -> str:
        self.audio_router.select_source("pi_dac")
        volume = float(payload.get("value", self.config["integrations"]["media"].get("startup_volume", 0.5)))
        bt_cfg = self.config.get("integrations", {}).get("bluetooth_audio", {})
        if isinstance(bt_cfg, dict) and bool(bt_cfg.get("enabled", False)):
            self.config["integrations"]["media"]["startup_volume"] = volume
            if self.media:
                self.media.set_volume(volume)
            return f"Bluetooth audio: {self.bluetooth_audio.set_media_volume(volume)}"
        if not self.media:
            return "Media unavailable"
        self.media.set_volume(volume)
        return f"Volume {volume:.2f}"

    def _cmd_bluetooth_audio_reconnect(self, _: dict[str, Any]) -> str:
        self.bluetooth_audio.reconnect()
        return f"Bluetooth audio: {self.bluetooth_audio.status()}"

    def _cmd_bluetooth_connect(self, _: dict[str, Any]) -> str:
        self.bluetooth_audio.reconnect()
        return f"Bluetooth: {self.bluetooth_audio.status()}"

    def _cmd_bluetooth_disconnect(self, _: dict[str, Any]) -> str:
        return f"Bluetooth: {self.bluetooth_audio.disconnect()}"

    def _cmd_radio_fm_mode(self, _: dict[str, Any]) -> str:
        if not self.radio:
            return "Radio unavailable"
        self.audio_router.select_source("fm_radio")
        return f"Radio: {self.radio.fm_mode()}"

    def _cmd_radio_wb_mode(self, payload: dict[str, Any]) -> str:
        if not self.radio:
            return "Radio unavailable"
        self.audio_router.select_source("fm_radio")
        channel = payload.get("value")
        try:
            parsed_channel = int(channel) if channel is not None else None
        except (TypeError, ValueError):
            parsed_channel = None
        return f"Radio: {self.radio.wb_mode(parsed_channel)}"

    def _cmd_radio_seek_up(self, _: dict[str, Any]) -> str:
        if not self.radio:
            return "Radio unavailable"
        self.audio_router.select_source("fm_radio")
        return f"Radio: {self.radio.seek_up()}"

    def _cmd_radio_seek_down(self, _: dict[str, Any]) -> str:
        if not self.radio:
            return "Radio unavailable"
        self.audio_router.select_source("fm_radio")
        return f"Radio: {self.radio.seek_down()}"

    def _cmd_radio_tune_up(self, _: dict[str, Any]) -> str:
        if not self.radio:
            return "Radio unavailable"
        self.audio_router.select_source("fm_radio")
        step = float(self.config["integrations"].get("fm_radio", {}).get("fm_step_mhz", 0.1))
        return f"Radio: {self.radio.tune_step(abs(step))}"

    def _cmd_radio_tune_down(self, _: dict[str, Any]) -> str:
        if not self.radio:
            return "Radio unavailable"
        self.audio_router.select_source("fm_radio")
        step = float(self.config["integrations"].get("fm_radio", {}).get("fm_step_mhz", 0.1))
        return f"Radio: {self.radio.tune_step(-abs(step))}"

    def _cmd_radio_scan_cycle(self, _: dict[str, Any]) -> str:
        if not self.radio:
            return "Radio unavailable"
        self.audio_router.select_source("fm_radio")
        return f"Radio: {self.radio.toggle_scan_cycle()}"

    def _cmd_radio_set_volume(self, payload: dict[str, Any]) -> str:
        if not self.radio:
            return "Radio unavailable"
        self.audio_router.select_source("fm_radio")
        default_volume = self.config["integrations"].get("fm_radio", {}).get("startup_volume", 45)
        try:
            volume = int(float(payload.get("value", default_volume)))
        except (TypeError, ValueError):
            volume = int(default_volume)
        return f"Radio: {self.radio.set_volume(volume)}"

    def _cmd_radio_mute_toggle(self, _: dict[str, Any]) -> str:
        if not self.radio:
            return "Radio unavailable"
        self.audio_router.select_source("fm_radio")
        return f"Radio: {self.radio.toggle_mute()}"

    def _cmd_audio_source_pi(self, _: dict[str, Any]) -> str:
        return f"Audio: {self.audio_router.select_source('pi_dac')}"

    def _cmd_audio_source_fm(self, _: dict[str, Any]) -> str:
        return f"Audio: {self.audio_router.select_source('fm_radio')}"

    def _cmd_audio_select_source(self, payload: dict[str, Any]) -> str:
        source = str(payload.get("value", "")).strip()
        if not source:
            source = str(self.config.get("integrations", {}).get("audio_router", {}).get("default_source", "pi_dac"))
        return f"Audio: {self.audio_router.select_source(source)}"

    def _cmd_app_restart(self, _: dict[str, Any]) -> str:
        self._running = False
        return "Stopping app (restart requested)"

    def _cmd_wifi_rescan(self, _: dict[str, Any]) -> str:
        return wifi_rescan()

    def _cmd_wifi_connect(self, _: dict[str, Any]) -> str:
        return wifi_connect(self.config)

    def _cmd_wifi_disconnect(self, _: dict[str, Any]) -> str:
        return wifi_disconnect(self.config)

    def _cmd_wifi_radio_toggle(self, _: dict[str, Any]) -> str:
        return wifi_radio_toggle()

    def _cmd_theme_next(self, _: dict[str, Any]) -> str:
        available = self._available_theme_names()
        if not available:
            return "Theme root missing"

        active = str(self.config["themes"].get("active", ""))
        if active in available:
            next_index = (available.index(active) + 1) % len(available)
        else:
            next_index = 0
        self.config["themes"]["active"] = available[next_index]
        self._refresh_theme_components()
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
        resolved_theme = load_theme_data(theme_root=theme_root, theme_name=theme_name)
        current_mode = self._center_mode_from_theme_data(resolved_theme)
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
        self._refresh_theme_components()
        return f"Center dial: {next_mode}"

    def _cmd_map_view_toggle(self, _: dict[str, Any]) -> str:
        current = bool(self.config.get("system", {}).get("map_view_visible", False))
        next_visible = not current
        self.config.setdefault("system", {})["map_view_visible"] = next_visible
        return f"Map view: {'on' if next_visible else 'off'}"

    def _cmd_app_main_screen(self, _: dict[str, Any]) -> str:
        self.config.setdefault("system", {})["map_view_visible"] = False
        if self.display_manager:
            self.display_manager.close_menu_overlay()
        return "Main dash screen"

    def _cmd_menu_open_back(self, _: dict[str, Any]) -> str:
        if not self.display_manager:
            return "Menu unavailable"
        return self.display_manager.menu_open_or_back()

    def _cmd_menu_close(self, _: dict[str, Any]) -> str:
        if not self.display_manager:
            return "Menu unavailable"
        self.display_manager.close_menu_overlay()
        return "Menu closed"

    def _cmd_faults_clear(self, _: dict[str, Any]) -> str:
        self._fault_latched_word = 0
        self._values["arduino.fault_latched_word"] = 0.0
        self._values["arduino.fault_latched_count"] = 0.0
        active_word = int(self._values.get("arduino.fault_word", 0.0))
        if active_word != 0:
            return "Faults cleared (active fault still present)"
        return "Faults cleared"

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
        resolved_theme = load_theme_data(theme_root=theme_root, theme_name=theme_name)
        if not resolved_theme:
            return "tach"
        return self._center_mode_from_theme_data(resolved_theme)

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
            if theme_name not in set(self._available_theme_names()):
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

    def _init_diagnostic_pin(self) -> None:
        self._diagnostic_pin = None
        self._diagnostic_pin_state = "-"
        system_cfg = self.config.get("system", {})
        pin = int(system_cfg.get("diagnostics_gpio_pin", -1))
        pull_up = bool(system_cfg.get("diagnostics_gpio_active_low", True))
        if pin < 0 or DigitalInputDevice is None:
            return
        try:
            self._diagnostic_pin = DigitalInputDevice(pin, pull_up=pull_up)
        except Exception:
            self._diagnostic_pin = None

    def _tick_power_links(self) -> None:
        self._values.update(self.attiny_power.poll())
        if self._diagnostic_pin is None:
            self._diagnostic_pin_state = "-"
            return
        try:
            active_low = bool(self.config.get("system", {}).get("diagnostics_gpio_active_low", True))
            raw = bool(self._diagnostic_pin.value)
            active = (not raw) if active_low else raw
            self._diagnostic_pin_state = "active" if active else "inactive"
            self._values["diagnostics.pin_active"] = 1.0 if active else 0.0
        except Exception:
            self._diagnostic_pin_state = "error"

    def _oled_clock_strings(self) -> tuple[str, str]:
        now = datetime.now()
        tm = now.strftime("%I:%M")
        if tm.startswith("0") and len(tm) > 1 and tm[1] != ":":
            tm = tm[1:]
        ap = now.strftime("%p").upper()
        return tm, ap

    def _oled_ambient_temp_display_int_hysteresis(self, temp_f: float) -> int:
        """Match gauge_screen clock: avoid 71/72 flicker when float hovers near a half-degree boundary."""
        last = self._oled_ambient_display_int
        if last is None:
            n = int(round(temp_f))
            self._oled_ambient_display_int = n
            return n
        if temp_f >= last + 0.5:
            n = int(math.floor(temp_f + 0.5))
        elif temp_f <= last - 0.5:
            n = int(math.ceil(temp_f - 0.5))
        else:
            n = last
        self._oled_ambient_display_int = n
        return n

    def _oled_weather_temp_str(self) -> str:
        """Encoder OLED WT token (NanoBridge downlink on encoder UART).

        Path: ATmega328 sensor_pack UART (Pi) → ArduinoSerialInput → InputManager.poll →
        DashboardApp._tick_inputs SensorReadingStability.apply → _values → this string →
        NanoBridge.send_oled_frame |WT=… → encoder_controller parseDownlinkLine WT.
        """
        if "analog_inputs.ambient_temp" not in self._values:
            return "--"
        temp_f = float(self._values["analog_inputs.ambient_temp"])
        shown = self._oled_ambient_temp_display_int_hysteresis(temp_f)
        return f"{shown}F"

    def _oled_media_for_nano(self, status: dict[str, str]) -> tuple[str, str, int, str, str]:
        """song_title, song_artist, progress_pct, pos_elapsed, pos_total."""
        bt_cfg = self.config.get("integrations", {}).get("bluetooth_audio", {})
        bt_on = isinstance(bt_cfg, dict) and bool(bt_cfg.get("enabled", False))
        if bt_on and self.bluetooth_audio:
            st = self.bluetooth_audio.media_title()
            sa = self.bluetooth_audio.media_artist()
            if not st and not sa:
                combined = (status.get("media_track", "") or "").strip()
                if " - " in combined:
                    parts = combined.split(" - ", 1)
                    sa, st = parts[0].strip(), parts[1].strip()
                elif combined and combined not in ("-", "disabled"):
                    st, sa = combined, ""
                else:
                    st, sa = "-", ""
            return (st or "-", sa or "-", 0, "0:00", "0:00")
        if self.media:
            return (
                self.media.track_title(),
                "Local",
                self.media.progress_pct(),
                self.media.elapsed_mmss(),
                "-:--",
            )
        return ("-", "-", 0, "0:00", "0:00")

    def _publish_oled_and_audio(self, status: dict[str, str]) -> None:
        track = status.get("media_track", "-")
        self.esp32_link.publish_metadata(track)
        nb_cfg = self.config.get("integrations", {}).get("nano_bridge", {})
        weather_label = str(nb_cfg.get("weather_label", "OUTSIDE")) if isinstance(nb_cfg, dict) else "OUTSIDE"
        st, sa, pr, pe, pt = self._oled_media_for_nano(status)
        tm, ap = self._oled_clock_strings()
        dm = self.display_manager
        menu_open = bool(dm and dm.menu_overlay_visible())
        pl = dm.oled_menu_nano_payload(status) if (menu_open and dm and dm.menu_render_on_oled()) else None
        if pl is not None:
            self.nano_bridge.send_oled_frame(
                mode="menu",
                title=str(pl.get("title", "-")),
                line1="-",
                line2="-",
                weather_label=weather_label,
                weather_temp=self._oled_weather_temp_str(),
                song_title=st,
                song_artist=sa,
                progress_pct=pr,
                pos_elapsed=pe,
                pos_total=pt,
                clock_time=tm,
                clock_ampm=ap,
                menu_rows=(str(pl.get("m0", "")), str(pl.get("m1", "")), str(pl.get("m2", ""))),
                menu_row_values=(
                    str(pl.get("v0", "")),
                    str(pl.get("v1", "")),
                    str(pl.get("v2", "")),
                ),
                menu_total=int(pl.get("total", 0)),
                menu_offset=int(pl.get("offset", 0)),
                menu_sel=int(pl.get("sel", 0)),
            )
        else:
            self.nano_bridge.send_oled_frame(
                mode="drive",
                title=status.get("theme_active", "-"),
                line1=status.get("media", "-"),
                line2=track,
                weather_label=weather_label,
                weather_temp=self._oled_weather_temp_str(),
                song_title=st,
                song_artist=sa,
                progress_pct=pr,
                pos_elapsed=pe,
                pos_total=pt,
                clock_time=tm,
                clock_ampm=ap,
            )
