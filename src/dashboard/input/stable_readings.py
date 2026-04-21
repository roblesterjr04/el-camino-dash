from __future__ import annotations

from typing import Any

DEFAULT_SENSOR_KEYS = frozenset(
    {
        "speed_sensor.vehicle_speed",
        "analog_inputs.engine_rpm",
        "analog_inputs.fuel_level",
        "analog_inputs.coolant_temp",
        "analog_inputs.oil_pressure",
        "indicators.oil",
        "analog_inputs.system_voltage",
        "indicators.alt_ok",
        "indicators.ign_on",
        "arduino.fault_word",
        "analog_inputs.ambient_temp",
        "sensor_pack.odometer_miles",
    }
)


class SensorReadingStability:
    """Holds each keyed value until it matches N times in a row (within epsilon)."""

    __slots__ = (
        "_candidate",
        "_enabled",
        "_epsilon",
        "_epsilon_by_key",
        "_keys",
        "_required",
        "_stable",
        "_streak",
    )

    def __init__(self, spec: dict[str, Any]) -> None:
        self._enabled = bool(spec.get("enabled", False))
        self._required = max(1, int(spec.get("required_samples", 7)))
        self._epsilon = max(0.0, float(spec.get("epsilon", 0.05)))
        raw_ebk = spec.get("epsilon_by_key")
        self._epsilon_by_key: dict[str, float] = {}
        if isinstance(raw_ebk, dict):
            for k, v in raw_ebk.items():
                self._epsilon_by_key[str(k)] = max(0.0, float(v))
        raw_keys = spec.get("keys")
        if isinstance(raw_keys, list) and raw_keys:
            self._keys = frozenset(str(k) for k in raw_keys)
        else:
            self._keys = DEFAULT_SENSOR_KEYS
        self._stable: dict[str, float] = {}
        self._candidate: dict[str, float] = {}
        self._streak: dict[str, int] = {}

    def is_enabled(self) -> bool:
        return self._enabled

    def _eps_for(self, key: str) -> float:
        return self._epsilon_by_key.get(key, self._epsilon)

    @staticmethod
    def _close(a: float, b: float, epsilon: float) -> bool:
        return abs(a - b) <= epsilon

    def apply(self, incoming: dict[str, float]) -> dict[str, float]:
        if not self._enabled or not incoming:
            return incoming
        out = dict(incoming)
        need = self._required
        for key in self._keys:
            if key not in incoming:
                continue
            v = float(incoming[key])
            eps = self._eps_for(key)
            stable = self._stable.get(key)

            if stable is not None and self._close(v, stable, eps):
                self._candidate[key] = v
                self._streak[key] = need
                out[key] = stable
                continue

            cand = self._candidate.get(key)
            if cand is None or not self._close(v, cand, eps):
                self._candidate[key] = v
                self._streak[key] = 1
            else:
                self._streak[key] = self._streak.get(key, 0) + 1

            if self._streak[key] >= need:
                self._stable[key] = float(self._candidate[key])

            if key in self._stable:
                out[key] = self._stable[key]
            else:
                out[key] = v
        return out
