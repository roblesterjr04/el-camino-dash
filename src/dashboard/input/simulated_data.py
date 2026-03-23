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

    def _blink(self, elapsed: float, period_s: float, duty_cycle: float) -> float:
        if period_s <= 0:
            return 0.0
        duty = max(0.01, min(0.99, duty_cycle))
        phase = (elapsed % period_s) / period_s
        return 1.0 if phase < duty else 0.0

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
        if "ambient_range" in self._spec:
            period = float(self._spec.get("ambient_period_s", self._spec.get("fuel_period_s", 24.0)))
            updates["analog_inputs.ambient_temp"] = self._wave(elapsed, period, self._spec["ambient_range"])
        if "amp_range" in self._spec:
            period = float(self._spec.get("amp_period_s", self._spec["rpm_period_s"]))
            updates["analog_inputs.system_amp"] = self._wave(elapsed, period, self._spec["amp_range"])
        if "oil_range" in self._spec:
            period = float(self._spec.get("oil_period_s", self._spec["fuel_period_s"]))
            updates["indicators.oil"] = self._wave(elapsed, period, self._spec["oil_range"])
        if bool(self._spec.get("turn_signals_enabled", False)):
            period = float(self._spec.get("turn_signal_period_s", 0.8))
            duty = float(self._spec.get("turn_signal_duty_cycle", 0.5))
            mode = str(self._spec.get("turn_signal_mode", "alternate")).strip().lower()
            blink_on = self._blink(elapsed, period, duty)
            if mode == "left":
                updates["indicators.turn_left"] = blink_on
                updates["indicators.turn_right"] = 0.0
            elif mode == "right":
                updates["indicators.turn_left"] = 0.0
                updates["indicators.turn_right"] = blink_on
            elif mode == "hazard":
                updates["indicators.turn_left"] = blink_on
                updates["indicators.turn_right"] = blink_on
            else:
                # Alternate mode creates visible motion in demo mode.
                updates["indicators.turn_left"] = blink_on
                updates["indicators.turn_right"] = 1.0 - blink_on
        if bool(self._spec.get("fault_sim_enabled", False)):
            codes_raw = self._spec.get("fault_codes", [1, 4, 16, 128, 256, 1024])
            codes = [int(code) for code in codes_raw if int(code) >= 0]
            if codes:
                cycle_period = max(0.2, float(self._spec.get("fault_cycle_period_s", 5.0)))
                phase = int(elapsed / cycle_period) % (len(codes) + 1)
                fault_word = 0 if phase == len(codes) else int(codes[phase])
                updates["arduino.fault_word"] = float(fault_word)
        return updates
