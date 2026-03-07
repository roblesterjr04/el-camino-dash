from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml


class ConfigError(Exception):
    """Raised when dashboard config is invalid."""


@dataclass(frozen=True)
class DashboardConfig:
    raw: dict[str, Any]
    path: Path

    @property
    def system(self) -> dict[str, Any]:
        return self.raw["system"]

    @property
    def displays(self) -> list[dict[str, Any]]:
        return self.raw["displays"]

    @property
    def themes(self) -> dict[str, Any]:
        return self.raw["themes"]

    @property
    def gauges(self) -> dict[str, dict[str, Any]]:
        return self.raw["gauges"]

    @property
    def inputs(self) -> dict[str, Any]:
        return self.raw["inputs"]

    @property
    def integrations(self) -> dict[str, Any]:
        return self.raw["integrations"]


REQUIRED_TOP_LEVEL = {"system", "displays", "themes", "gauges", "inputs", "integrations"}
ALLOWED_TOP_LEVEL = REQUIRED_TOP_LEVEL


def _fail(path: str, message: str) -> None:
    raise ConfigError(f"{path}: {message}")


def _expect_keys(path: str, obj: dict[str, Any], required: set[str], allowed: set[str] | None = None) -> None:
    for key in required:
        if key not in obj:
            _fail(path, f"missing key '{key}'")
    if allowed is not None:
        for key in obj:
            if key not in allowed:
                _fail(path, f"unknown key '{key}'")


def _expect_type(path: str, value: Any, expected_type: type | tuple[type, ...]) -> None:
    if not isinstance(value, expected_type):
        _fail(path, f"expected {expected_type}, got {type(value)}")


def _validate_display(idx: int, display: dict[str, Any]) -> None:
    path = f"displays[{idx}]"
    required = {"id", "enabled", "shape", "width", "height", "position", "fps", "role"}
    allowed = required | {"gauge_id", "monitor_index", "windowed", "fullscreen"}
    _expect_keys(path, display, required, allowed)
    _expect_type(f"{path}.id", display["id"], str)
    _expect_type(f"{path}.enabled", display["enabled"], bool)
    _expect_type(f"{path}.shape", display["shape"], str)
    if display["shape"] not in {"round", "rectangle"}:
        _fail(f"{path}.shape", "must be 'round' or 'rectangle'")
    _expect_type(f"{path}.width", display["width"], int)
    _expect_type(f"{path}.height", display["height"], int)
    _expect_type(f"{path}.position", display["position"], list)
    if len(display["position"]) != 2:
        _fail(f"{path}.position", "must contain [x, y]")
    _expect_type(f"{path}.position[0]", display["position"][0], int)
    _expect_type(f"{path}.position[1]", display["position"][1], int)
    _expect_type(f"{path}.fps", display["fps"], int)
    _expect_type(f"{path}.role", display["role"], str)
    if display["role"] != "gauge":
        _fail(f"{path}.role", "must be 'gauge'")
    if display["role"] == "gauge" and "gauge_id" not in display:
        _fail(path, "gauge displays require 'gauge_id'")
    if "monitor_index" in display:
        _expect_type(f"{path}.monitor_index", display["monitor_index"], int)
        if int(display["monitor_index"]) < 0:
            _fail(f"{path}.monitor_index", "must be >= 0")
    windowed = bool(display.get("windowed", True))
    fullscreen = bool(display.get("fullscreen", False))
    if "windowed" in display:
        _expect_type(f"{path}.windowed", display["windowed"], bool)
    if "fullscreen" in display:
        _expect_type(f"{path}.fullscreen", display["fullscreen"], bool)
    if windowed and fullscreen:
        _fail(path, "windowed and fullscreen cannot both be true")
    if not windowed and not fullscreen:
        _fail(path, "windowed=false requires fullscreen=true")


def _validate_gauges(gauges: dict[str, Any]) -> None:
    _expect_type("gauges", gauges, dict)
    for gauge_id, spec in gauges.items():
        path = f"gauges.{gauge_id}"
        _expect_type(path, spec, dict)
        required = {"label", "unit", "source", "min_value", "max_value", "smoothing_alpha"}
        allowed = required | {
            "secondary_label",
            "secondary_source",
            "secondary_unit",
            "odometer_source",
            "odometer_label",
            "odometer_unit",
            "odometer_decimals",
        }
        _expect_keys(path, spec, required, allowed)
        _expect_type(f"{path}.label", spec["label"], str)
        _expect_type(f"{path}.unit", spec["unit"], str)
        _expect_type(f"{path}.source", spec["source"], str)
        _expect_type(f"{path}.min_value", spec["min_value"], (int, float))
        _expect_type(f"{path}.max_value", spec["max_value"], (int, float))
        _expect_type(f"{path}.smoothing_alpha", spec["smoothing_alpha"], (int, float))
        if "secondary_label" in spec:
            _expect_type(f"{path}.secondary_label", spec["secondary_label"], str)
        if "secondary_source" in spec:
            _expect_type(f"{path}.secondary_source", spec["secondary_source"], str)
        if "secondary_unit" in spec:
            _expect_type(f"{path}.secondary_unit", spec["secondary_unit"], str)
        if "odometer_source" in spec:
            _expect_type(f"{path}.odometer_source", spec["odometer_source"], str)
        if "odometer_label" in spec:
            _expect_type(f"{path}.odometer_label", spec["odometer_label"], str)
        if "odometer_unit" in spec:
            _expect_type(f"{path}.odometer_unit", spec["odometer_unit"], str)
        if "odometer_decimals" in spec:
            _expect_type(f"{path}.odometer_decimals", spec["odometer_decimals"], int)


def _validate_inputs(inputs: dict[str, Any]) -> None:
    required_sections = {"rotary_encoders", "analog_inputs", "speed_sensors", "simulated_data", "keyboard_sim"}
    allowed_sections = required_sections | {"gpio_buttons", "mcp23017_controls", "arduino_serial", "ir_remote"}
    _expect_keys("inputs", inputs, required_sections, allowed_sections)
    for section in ("rotary_encoders", "analog_inputs", "speed_sensors"):
        _expect_type(f"inputs.{section}", inputs[section], list)
        for idx, item in enumerate(inputs[section]):
            _expect_type(f"inputs.{section}[{idx}]", item, dict)
            if "id" not in item:
                _fail(f"inputs.{section}[{idx}]", "missing key 'id'")
            if section == "rotary_encoders" and "button_debounce_ms" in item:
                _expect_type(f"inputs.{section}[{idx}].button_debounce_ms", item["button_debounce_ms"], (int, float))
    if "arduino_serial" in inputs:
        _expect_type("inputs.arduino_serial", inputs["arduino_serial"], dict)
        _expect_keys(
            "inputs.arduino_serial",
            inputs["arduino_serial"],
            {"enabled"},
            {
                "enabled",
                "port",
                "baudrate",
                "timeout_s",
                "wheel_circumference_m",
                "speed_pulses_per_rev",
                "fuel_range",
                "temp_range",
                "oil_range",
                "voltage_range",
                "fuel_raw_range",
                "temp_raw_range",
                "oil_raw_range",
                "voltage_raw_range",
            },
        )
        _expect_type("inputs.arduino_serial.enabled", inputs["arduino_serial"]["enabled"], bool)
        if "port" in inputs["arduino_serial"]:
            _expect_type("inputs.arduino_serial.port", inputs["arduino_serial"]["port"], str)
        if "baudrate" in inputs["arduino_serial"]:
            _expect_type("inputs.arduino_serial.baudrate", inputs["arduino_serial"]["baudrate"], int)
        if "timeout_s" in inputs["arduino_serial"]:
            _expect_type("inputs.arduino_serial.timeout_s", inputs["arduino_serial"]["timeout_s"], (int, float))
        if "wheel_circumference_m" in inputs["arduino_serial"]:
            _expect_type(
                "inputs.arduino_serial.wheel_circumference_m",
                inputs["arduino_serial"]["wheel_circumference_m"],
                (int, float),
            )
        if "speed_pulses_per_rev" in inputs["arduino_serial"]:
            _expect_type(
                "inputs.arduino_serial.speed_pulses_per_rev",
                inputs["arduino_serial"]["speed_pulses_per_rev"],
                (int, float),
            )
        for key in (
            "fuel_range",
            "temp_range",
            "oil_range",
            "voltage_range",
            "fuel_raw_range",
            "temp_raw_range",
            "oil_raw_range",
            "voltage_raw_range",
        ):
            if key not in inputs["arduino_serial"]:
                continue
            path = f"inputs.arduino_serial.{key}"
            _expect_type(path, inputs["arduino_serial"][key], list)
            if len(inputs["arduino_serial"][key]) != 2:
                _fail(path, "must contain [min, max]")
            _expect_type(f"{path}[0]", inputs["arduino_serial"][key][0], (int, float))
            _expect_type(f"{path}[1]", inputs["arduino_serial"][key][1], (int, float))
    _expect_type("inputs.keyboard_sim", inputs["keyboard_sim"], dict)
    _expect_keys(
        "inputs.keyboard_sim",
        inputs["keyboard_sim"],
        {"enabled", "mappings"},
        {
            "enabled",
            "mappings",
            "rotate_repeat_ms",
            "rpm_max",
            "temp_min",
            "temp_max",
            "amp_min",
            "amp_max",
            "oil_min",
            "oil_max",
        },
    )
    _expect_type("inputs.keyboard_sim.enabled", inputs["keyboard_sim"]["enabled"], bool)
    _expect_type("inputs.keyboard_sim.mappings", inputs["keyboard_sim"]["mappings"], dict)
    if "rotate_repeat_ms" in inputs["keyboard_sim"]:
        _expect_type("inputs.keyboard_sim.rotate_repeat_ms", inputs["keyboard_sim"]["rotate_repeat_ms"], (int, float))
    if "rpm_max" in inputs["keyboard_sim"]:
        _expect_type("inputs.keyboard_sim.rpm_max", inputs["keyboard_sim"]["rpm_max"], (int, float))
    if "temp_min" in inputs["keyboard_sim"]:
        _expect_type("inputs.keyboard_sim.temp_min", inputs["keyboard_sim"]["temp_min"], (int, float))
    if "temp_max" in inputs["keyboard_sim"]:
        _expect_type("inputs.keyboard_sim.temp_max", inputs["keyboard_sim"]["temp_max"], (int, float))
    if "amp_min" in inputs["keyboard_sim"]:
        _expect_type("inputs.keyboard_sim.amp_min", inputs["keyboard_sim"]["amp_min"], (int, float))
    if "amp_max" in inputs["keyboard_sim"]:
        _expect_type("inputs.keyboard_sim.amp_max", inputs["keyboard_sim"]["amp_max"], (int, float))
    if "oil_min" in inputs["keyboard_sim"]:
        _expect_type("inputs.keyboard_sim.oil_min", inputs["keyboard_sim"]["oil_min"], (int, float))
    if "oil_max" in inputs["keyboard_sim"]:
        _expect_type("inputs.keyboard_sim.oil_max", inputs["keyboard_sim"]["oil_max"], (int, float))

    if "gpio_buttons" in inputs:
        _expect_type("inputs.gpio_buttons", inputs["gpio_buttons"], dict)
        _expect_keys(
            "inputs.gpio_buttons",
            inputs["gpio_buttons"],
            {"enabled"},
            {"enabled", "menu_left_pin", "menu_select_pin", "pull_up", "left_delta", "debounce_ms"},
        )
        _expect_type("inputs.gpio_buttons.enabled", inputs["gpio_buttons"]["enabled"], bool)
        if "menu_left_pin" in inputs["gpio_buttons"]:
            _expect_type("inputs.gpio_buttons.menu_left_pin", inputs["gpio_buttons"]["menu_left_pin"], int)
        if "menu_select_pin" in inputs["gpio_buttons"]:
            _expect_type("inputs.gpio_buttons.menu_select_pin", inputs["gpio_buttons"]["menu_select_pin"], int)
        if "pull_up" in inputs["gpio_buttons"]:
            _expect_type("inputs.gpio_buttons.pull_up", inputs["gpio_buttons"]["pull_up"], bool)
        if "left_delta" in inputs["gpio_buttons"]:
            _expect_type("inputs.gpio_buttons.left_delta", inputs["gpio_buttons"]["left_delta"], (int, float))
        if "debounce_ms" in inputs["gpio_buttons"]:
            _expect_type("inputs.gpio_buttons.debounce_ms", inputs["gpio_buttons"]["debounce_ms"], (int, float))

    if "mcp23017_controls" in inputs:
        _expect_type("inputs.mcp23017_controls", inputs["mcp23017_controls"], dict)
        _expect_keys(
            "inputs.mcp23017_controls",
            inputs["mcp23017_controls"],
            {"enabled"},
            {
                "enabled",
                "i2c_bus",
                "address",
                "pull_up",
                "debounce_ms",
                "rotary_debounce_ms",
                "rotary_counts_per_step",
                "button_debounce_ms",
                "left_delta",
                "menu_pin_a",
                "menu_pin_b",
                "menu_pin_button",
                "aux_pin_a",
                "aux_pin_b",
                "aux_pin_button",
                "menu_left_pin",
                "menu_select_pin",
                "menu_acceleration",
                "aux_acceleration",
                "invert_menu_direction",
                "invert_aux_direction",
            },
        )
        _expect_type("inputs.mcp23017_controls.enabled", inputs["mcp23017_controls"]["enabled"], bool)
        for key in (
            "i2c_bus",
            "address",
            "menu_pin_a",
            "menu_pin_b",
            "menu_pin_button",
            "aux_pin_a",
            "aux_pin_b",
            "aux_pin_button",
            "menu_left_pin",
            "menu_select_pin",
        ):
            if key in inputs["mcp23017_controls"]:
                _expect_type(f"inputs.mcp23017_controls.{key}", inputs["mcp23017_controls"][key], int)
        for key in ("pull_up", "invert_menu_direction", "invert_aux_direction"):
            if key in inputs["mcp23017_controls"]:
                _expect_type(f"inputs.mcp23017_controls.{key}", inputs["mcp23017_controls"][key], bool)
        for key in (
            "debounce_ms",
            "rotary_debounce_ms",
            "button_debounce_ms",
            "rotary_counts_per_step",
            "left_delta",
            "menu_acceleration",
            "aux_acceleration",
        ):
            if key in inputs["mcp23017_controls"]:
                _expect_type(f"inputs.mcp23017_controls.{key}", inputs["mcp23017_controls"][key], (int, float))

    if "ir_remote" in inputs:
        _expect_type("inputs.ir_remote", inputs["ir_remote"], dict)
        _expect_keys(
            "inputs.ir_remote",
            inputs["ir_remote"],
            {"enabled"},
            {
                "enabled",
                "profile_path",
                "device_path",
                "device_name",
                "allow_repeats",
                "diagnostics_pulse_count",
                "diagnostics_window_ms",
                "diagnostics_max_gap_ms",
            },
        )
        _expect_type("inputs.ir_remote.enabled", inputs["ir_remote"]["enabled"], bool)
        if "profile_path" in inputs["ir_remote"]:
            _expect_type("inputs.ir_remote.profile_path", inputs["ir_remote"]["profile_path"], str)
        if "device_path" in inputs["ir_remote"]:
            _expect_type("inputs.ir_remote.device_path", inputs["ir_remote"]["device_path"], str)
        if "device_name" in inputs["ir_remote"]:
            _expect_type("inputs.ir_remote.device_name", inputs["ir_remote"]["device_name"], str)
        if "allow_repeats" in inputs["ir_remote"]:
            _expect_type("inputs.ir_remote.allow_repeats", inputs["ir_remote"]["allow_repeats"], bool)
        if "diagnostics_pulse_count" in inputs["ir_remote"]:
            _expect_type("inputs.ir_remote.diagnostics_pulse_count", inputs["ir_remote"]["diagnostics_pulse_count"], int)
        if "diagnostics_window_ms" in inputs["ir_remote"]:
            _expect_type("inputs.ir_remote.diagnostics_window_ms", inputs["ir_remote"]["diagnostics_window_ms"], (int, float))
        if "diagnostics_max_gap_ms" in inputs["ir_remote"]:
            _expect_type(
                "inputs.ir_remote.diagnostics_max_gap_ms",
                inputs["ir_remote"]["diagnostics_max_gap_ms"],
                (int, float),
            )

    _expect_type("inputs.simulated_data", inputs["simulated_data"], dict)
    _expect_keys(
        "inputs.simulated_data",
        inputs["simulated_data"],
        {"enabled", "speed_period_s", "rpm_period_s", "fuel_period_s", "speed_range", "rpm_range", "fuel_range"},
        {
            "enabled",
            "speed_period_s",
            "rpm_period_s",
            "fuel_period_s",
            "speed_range",
            "rpm_range",
            "fuel_range",
            "temp_period_s",
            "amp_period_s",
            "oil_period_s",
            "temp_range",
            "amp_range",
            "oil_range",
            "turn_signals_enabled",
            "turn_signal_period_s",
            "turn_signal_duty_cycle",
            "turn_signal_mode",
            "fault_sim_enabled",
            "fault_cycle_period_s",
            "fault_codes",
        },
    )
    _expect_type("inputs.simulated_data.enabled", inputs["simulated_data"]["enabled"], bool)
    _expect_type("inputs.simulated_data.speed_period_s", inputs["simulated_data"]["speed_period_s"], (int, float))
    _expect_type("inputs.simulated_data.rpm_period_s", inputs["simulated_data"]["rpm_period_s"], (int, float))
    _expect_type("inputs.simulated_data.fuel_period_s", inputs["simulated_data"]["fuel_period_s"], (int, float))
    if "temp_period_s" in inputs["simulated_data"]:
        _expect_type("inputs.simulated_data.temp_period_s", inputs["simulated_data"]["temp_period_s"], (int, float))
    if "amp_period_s" in inputs["simulated_data"]:
        _expect_type("inputs.simulated_data.amp_period_s", inputs["simulated_data"]["amp_period_s"], (int, float))
    if "oil_period_s" in inputs["simulated_data"]:
        _expect_type("inputs.simulated_data.oil_period_s", inputs["simulated_data"]["oil_period_s"], (int, float))
    if "turn_signals_enabled" in inputs["simulated_data"]:
        _expect_type("inputs.simulated_data.turn_signals_enabled", inputs["simulated_data"]["turn_signals_enabled"], bool)
    if "turn_signal_period_s" in inputs["simulated_data"]:
        _expect_type("inputs.simulated_data.turn_signal_period_s", inputs["simulated_data"]["turn_signal_period_s"], (int, float))
    if "turn_signal_duty_cycle" in inputs["simulated_data"]:
        _expect_type(
            "inputs.simulated_data.turn_signal_duty_cycle",
            inputs["simulated_data"]["turn_signal_duty_cycle"],
            (int, float),
        )
    if "turn_signal_mode" in inputs["simulated_data"]:
        _expect_type("inputs.simulated_data.turn_signal_mode", inputs["simulated_data"]["turn_signal_mode"], str)
    if "fault_sim_enabled" in inputs["simulated_data"]:
        _expect_type("inputs.simulated_data.fault_sim_enabled", inputs["simulated_data"]["fault_sim_enabled"], bool)
    if "fault_cycle_period_s" in inputs["simulated_data"]:
        _expect_type("inputs.simulated_data.fault_cycle_period_s", inputs["simulated_data"]["fault_cycle_period_s"], (int, float))
    if "fault_codes" in inputs["simulated_data"]:
        _expect_type("inputs.simulated_data.fault_codes", inputs["simulated_data"]["fault_codes"], list)
        for idx, code in enumerate(inputs["simulated_data"]["fault_codes"]):
            _expect_type(f"inputs.simulated_data.fault_codes[{idx}]", code, int)
    for key in ("speed_range", "rpm_range", "fuel_range"):
        path = f"inputs.simulated_data.{key}"
        _expect_type(path, inputs["simulated_data"][key], list)
        if len(inputs["simulated_data"][key]) != 2:
            _fail(path, "must contain [min, max]")
        _expect_type(f"{path}[0]", inputs["simulated_data"][key][0], (int, float))
        _expect_type(f"{path}[1]", inputs["simulated_data"][key][1], (int, float))
    for key in ("temp_range", "amp_range", "oil_range"):
        if key not in inputs["simulated_data"]:
            continue
        path = f"inputs.simulated_data.{key}"
        _expect_type(path, inputs["simulated_data"][key], list)
        if len(inputs["simulated_data"][key]) != 2:
            _fail(path, "must contain [min, max]")
        _expect_type(f"{path}[0]", inputs["simulated_data"][key][0], (int, float))
        _expect_type(f"{path}[1]", inputs["simulated_data"][key][1], (int, float))


def _validate_integrations(integrations: dict[str, Any]) -> None:
    required_sections = {"navit", "media", "bluetooth_metadata"}
    allowed_sections = required_sections | {"bluetooth_audio", "fm_radio", "audio_router"}
    _expect_keys("integrations", integrations, required_sections, allowed_sections)
    for section in required_sections:
        _expect_type(f"integrations.{section}", integrations[section], dict)
        if "enabled" not in integrations[section]:
            _fail(f"integrations.{section}", "missing key 'enabled'")
        _expect_type(f"integrations.{section}.enabled", integrations[section]["enabled"], bool)
    if "bluetooth_audio" in integrations:
        bt_audio = integrations["bluetooth_audio"]
        _expect_type("integrations.bluetooth_audio", bt_audio, dict)
        _expect_keys(
            "integrations.bluetooth_audio",
            bt_audio,
            {"enabled"},
            {
                "enabled",
                "device_mac",
                "auto_connect",
                "pair_on_connect",
                "trust_device",
                "set_default_sink",
                "sink_name",
                "refresh_interval_s",
            },
        )
        _expect_type("integrations.bluetooth_audio.enabled", bt_audio["enabled"], bool)
        if "device_mac" in bt_audio:
            _expect_type("integrations.bluetooth_audio.device_mac", bt_audio["device_mac"], str)
        if "auto_connect" in bt_audio:
            _expect_type("integrations.bluetooth_audio.auto_connect", bt_audio["auto_connect"], bool)
        if "pair_on_connect" in bt_audio:
            _expect_type("integrations.bluetooth_audio.pair_on_connect", bt_audio["pair_on_connect"], bool)
        if "trust_device" in bt_audio:
            _expect_type("integrations.bluetooth_audio.trust_device", bt_audio["trust_device"], bool)
        if "set_default_sink" in bt_audio:
            _expect_type("integrations.bluetooth_audio.set_default_sink", bt_audio["set_default_sink"], bool)
        if "sink_name" in bt_audio:
            _expect_type("integrations.bluetooth_audio.sink_name", bt_audio["sink_name"], str)
        if "refresh_interval_s" in bt_audio:
            _expect_type("integrations.bluetooth_audio.refresh_interval_s", bt_audio["refresh_interval_s"], (int, float))
    if "fm_radio" in integrations:
        fm_radio = integrations["fm_radio"]
        _expect_type("integrations.fm_radio", fm_radio, dict)
        _expect_keys(
            "integrations.fm_radio",
            fm_radio,
            {"enabled"},
            {
                "enabled",
                "chip",
                "i2c_bus",
                "address",
                "rst_pin",
                "startup_mode",
                "startup_freq_mhz",
                "wb_channel",
                "startup_volume",
                "fm_step_mhz",
            },
        )
        _expect_type("integrations.fm_radio.enabled", fm_radio["enabled"], bool)
        if "chip" in fm_radio:
            _expect_type("integrations.fm_radio.chip", fm_radio["chip"], str)
        if "i2c_bus" in fm_radio:
            _expect_type("integrations.fm_radio.i2c_bus", fm_radio["i2c_bus"], int)
        if "address" in fm_radio:
            _expect_type("integrations.fm_radio.address", fm_radio["address"], int)
        if "rst_pin" in fm_radio:
            _expect_type("integrations.fm_radio.rst_pin", fm_radio["rst_pin"], int)
        if "startup_mode" in fm_radio:
            _expect_type("integrations.fm_radio.startup_mode", fm_radio["startup_mode"], str)
        if "startup_freq_mhz" in fm_radio:
            _expect_type("integrations.fm_radio.startup_freq_mhz", fm_radio["startup_freq_mhz"], (int, float))
        if "wb_channel" in fm_radio:
            _expect_type("integrations.fm_radio.wb_channel", fm_radio["wb_channel"], int)
        if "startup_volume" in fm_radio:
            _expect_type("integrations.fm_radio.startup_volume", fm_radio["startup_volume"], (int, float))
        if "fm_step_mhz" in fm_radio:
            _expect_type("integrations.fm_radio.fm_step_mhz", fm_radio["fm_step_mhz"], (int, float))
    if "audio_router" in integrations:
        audio_router = integrations["audio_router"]
        _expect_type("integrations.audio_router", audio_router, dict)
        _expect_keys(
            "integrations.audio_router",
            audio_router,
            {"enabled"},
            {
                "enabled",
                "chip",
                "protocol",
                "i2c_bus",
                "address",
                "default_source",
                "inter_command_delay_s",
                "settle_ms",
                "sources",
            },
        )
        _expect_type("integrations.audio_router.enabled", audio_router["enabled"], bool)
        if "chip" in audio_router:
            _expect_type("integrations.audio_router.chip", audio_router["chip"], str)
        if "protocol" in audio_router:
            _expect_type("integrations.audio_router.protocol", audio_router["protocol"], str)
        if "i2c_bus" in audio_router:
            _expect_type("integrations.audio_router.i2c_bus", audio_router["i2c_bus"], int)
        if "address" in audio_router:
            _expect_type("integrations.audio_router.address", audio_router["address"], int)
        if "default_source" in audio_router:
            _expect_type("integrations.audio_router.default_source", audio_router["default_source"], str)
        if "inter_command_delay_s" in audio_router:
            _expect_type(
                "integrations.audio_router.inter_command_delay_s",
                audio_router["inter_command_delay_s"],
                (int, float),
            )
        if "settle_ms" in audio_router:
            _expect_type("integrations.audio_router.settle_ms", audio_router["settle_ms"], (int, float))
        if "sources" in audio_router:
            _expect_type("integrations.audio_router.sources", audio_router["sources"], dict)
            for source_id, source_spec in audio_router["sources"].items():
                base = f"integrations.audio_router.sources.{source_id}"
                _expect_type(base, source_spec, dict)
                _expect_keys(base, source_spec, {"command_bytes"}, {"command_bytes"})
                _expect_type(f"{base}.command_bytes", source_spec["command_bytes"], list)
                for idx, command_value in enumerate(source_spec["command_bytes"]):
                    _expect_type(f"{base}.command_bytes[{idx}]", command_value, (int, str))


def _validate_system(system: dict[str, Any]) -> None:
    _expect_type("system", system, dict)
    if "spi_displays" not in system:
        return
    _expect_type("system.spi_displays", system["spi_displays"], list)
    for idx, display in enumerate(system["spi_displays"]):
        path = f"system.spi_displays[{idx}]"
        _expect_type(path, display, dict)
        required = {"spi_bus", "spi_device", "label"}
        allowed = required | {"dc", "rst", "dc_pin", "rst_pin", "rotation", "speed_hz", "madctl", "colmod"}
        _expect_keys(path, display, required, allowed)
        _expect_type(f"{path}.spi_bus", display["spi_bus"], int)
        _expect_type(f"{path}.spi_device", display["spi_device"], int)
        _expect_type(f"{path}.label", display["label"], str)
        if "dc_pin" in display:
            _expect_type(f"{path}.dc_pin", display["dc_pin"], int)
        if "rst_pin" in display:
            _expect_type(f"{path}.rst_pin", display["rst_pin"], int)
        if "dc" in display:
            _expect_type(f"{path}.dc", display["dc"], int)
        if "rst" in display:
            _expect_type(f"{path}.rst", display["rst"], int)
        if "dc_pin" not in display and "dc" not in display:
            _fail(path, "requires either 'dc_pin' or 'dc'")
        if "rst_pin" not in display and "rst" not in display:
            _fail(path, "requires either 'rst_pin' or 'rst'")
        if "rotation" in display:
            _expect_type(f"{path}.rotation", display["rotation"], int)
        if "speed_hz" in display:
            _expect_type(f"{path}.speed_hz", display["speed_hz"], int)
        if "madctl" in display:
            _expect_type(f"{path}.madctl", display["madctl"], int)
        if "colmod" in display:
            _expect_type(f"{path}.colmod", display["colmod"], int)


def validate_config(raw: dict[str, Any]) -> None:
    _expect_keys("root", raw, REQUIRED_TOP_LEVEL, ALLOWED_TOP_LEVEL)
    _expect_type("system", raw["system"], dict)
    _expect_type("displays", raw["displays"], list)
    _expect_type("themes", raw["themes"], dict)
    _expect_type("gauges", raw["gauges"], dict)
    _expect_type("inputs", raw["inputs"], dict)
    _expect_type("integrations", raw["integrations"], dict)
    _validate_system(raw["system"])

    for idx, display in enumerate(raw["displays"]):
        _expect_type(f"displays[{idx}]", display, dict)
        _validate_display(idx, display)
    enabled_display_ids = [str(display["id"]) for display in raw["displays"] if bool(display.get("enabled", False))]
    display_ids = set(enabled_display_ids)
    enabled_display_count = len(enabled_display_ids)
    menu_display_id = raw["system"].get("menu_display_id", "gauge_right")
    _expect_type("system.menu_display_id", menu_display_id, str)
    if enabled_display_ids and (not menu_display_id or menu_display_id not in display_ids):
        # Be resilient across layouts/themes: if configured menu target is not
        # present, pin it to the first enabled display instead of failing load.
        raw["system"]["menu_display_id"] = enabled_display_ids[0]
    if "spi_displays" in raw["system"]:
        spi_displays = raw["system"]["spi_displays"]
        if len(spi_displays) < enabled_display_count:
            _fail(
                "system.spi_displays",
                f"must include at least {enabled_display_count} entries for enabled displays",
            )

    _validate_gauges(raw["gauges"])
    _validate_inputs(raw["inputs"])
    _validate_integrations(raw["integrations"])


def load_dashboard_config(config_path: str | Path) -> DashboardConfig:
    path = Path(config_path)
    if not path.exists():
        raise ConfigError(f"Config file not found: {path}")

    with path.open("r", encoding="utf-8") as handle:
        raw = yaml.safe_load(handle)

    if not isinstance(raw, dict):
        raise ConfigError("root: expected mapping/object")

    validate_config(raw)
    return DashboardConfig(raw=raw, path=path)
