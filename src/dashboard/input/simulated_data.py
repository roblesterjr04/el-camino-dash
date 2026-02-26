from __future__ import annotations

import math
import time
from typing import Any


class SimulatedDataInput:
    def __init__(self, spec: dict[str, Any]) -> None:
        self._spec = spec
        self._start = time.monotonic()

    def _wave(self, elapsed: float, period_s: float, value_range: list[float]) -> float:
        if period_s <= 0:
            return float(value_range[0])
        lo = float(value_range[0])
        hi = float(value_range[1])
        midpoint = (lo + hi) / 2.0
        amplitude = (hi - lo) / 2.0
        phase = (elapsed / period_s) * (2.0 * math.pi)
        return midpoint + (math.sin(phase) * amplitude)

    def poll(self) -> dict[str, float]:
        if not self._spec.get("enabled", False):
            return {}

        elapsed = time.monotonic() - self._start
        speed = self._wave(elapsed, float(self._spec["speed_period_s"]), self._spec["speed_range"])
        rpm = self._wave(elapsed, float(self._spec["rpm_period_s"]), self._spec["rpm_range"])
        fuel = self._wave(elapsed, float(self._spec["fuel_period_s"]), self._spec["fuel_range"])
        updates = {
            "speed_sensor.vehicle_speed": speed,
            "analog_inputs.engine_rpm": rpm,
            "analog_inputs.fuel_level": fuel,
        }
        if "temp_range" in self._spec:
            period = float(self._spec.get("temp_period_s", self._spec["fuel_period_s"]))
            updates["analog_inputs.coolant_temp"] = self._wave(elapsed, period, self._spec["temp_range"])
        if "amp_range" in self._spec:
            period = float(self._spec.get("amp_period_s", self._spec["rpm_period_s"]))
            updates["analog_inputs.system_amp"] = self._wave(elapsed, period, self._spec["amp_range"])
        if "oil_range" in self._spec:
            period = float(self._spec.get("oil_period_s", self._spec["fuel_period_s"]))
            updates["indicators.oil"] = self._wave(elapsed, period, self._spec["oil_range"])
        return updates
