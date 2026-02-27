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
    _expect_keys("inputs", inputs, required_sections, required_sections)
    for section in ("rotary_encoders", "analog_inputs", "speed_sensors"):
        _expect_type(f"inputs.{section}", inputs[section], list)
        for idx, item in enumerate(inputs[section]):
            _expect_type(f"inputs.{section}[{idx}]", item, dict)
            if "id" not in item:
                _fail(f"inputs.{section}[{idx}]", "missing key 'id'")
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
    _expect_keys("integrations", integrations, required_sections, required_sections)
    for section in required_sections:
        _expect_type(f"integrations.{section}", integrations[section], dict)
        if "enabled" not in integrations[section]:
            _fail(f"integrations.{section}", "missing key 'enabled'")
        _expect_type(f"integrations.{section}.enabled", integrations[section]["enabled"], bool)


def validate_config(raw: dict[str, Any]) -> None:
    _expect_keys("root", raw, REQUIRED_TOP_LEVEL, ALLOWED_TOP_LEVEL)
    _expect_type("system", raw["system"], dict)
    _expect_type("displays", raw["displays"], list)
    _expect_type("themes", raw["themes"], dict)
    _expect_type("gauges", raw["gauges"], dict)
    _expect_type("inputs", raw["inputs"], dict)
    _expect_type("integrations", raw["integrations"], dict)

    for idx, display in enumerate(raw["displays"]):
        _expect_type(f"displays[{idx}]", display, dict)
        _validate_display(idx, display)

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
