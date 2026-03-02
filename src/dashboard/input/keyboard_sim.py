from __future__ import annotations

from dataclasses import dataclass
import time
from typing import Any

import pygame


_KEY_LOOKUP = {name: getattr(pygame, name) for name in dir(pygame) if name.startswith("K_")}


@dataclass
class KeyboardSimState:
    speed: float = 0.0
    rpm: float = 0.0
    fuel: float = 50.0
    temp: float = 180.0
    amp: float = 0.0
    oil: float = 100.0
    encoder_delta: float = 0.0
    encoder_presses: float = 0.0
    aux_encoder_delta: float = 0.0
    aux_encoder_presses: float = 0.0
    turn_left: float = 0.0
    turn_right: float = 0.0


class KeyboardSimInput:
    def __init__(self, config: dict[str, Any]) -> None:
        self._cfg = config
        self._state = KeyboardSimState()
        self._repeat_s = max(0.02, float(config.get("rotate_repeat_ms", 120)) / 1000.0)
        self._rpm_max = max(0.0, float(config.get("rpm_max", 8000.0)))
        self._temp_min = float(config.get("temp_min", 100.0))
        self._temp_max = float(config.get("temp_max", 260.0))
        self._amp_min = float(config.get("amp_min", -100.0))
        self._amp_max = float(config.get("amp_max", 100.0))
        self._oil_min = float(config.get("oil_min", 0.0))
        self._oil_max = float(config.get("oil_max", 100.0))
        self._last_emit_s: dict[str, float] = {"menu": 0.0, "aux": 0.0}
        self._last_pressed: dict[str, bool] = {
            "menu_press": False,
            "aux_press": False,
            "turn_left_toggle": False,
            "turn_right_toggle": False,
            "hazard_toggle": False,
        }

    def _key(self, mapping_name: str) -> int | None:
        key_name = self._cfg["mappings"].get(mapping_name)
        return _KEY_LOOKUP.get(key_name) if key_name else None

    def poll(self, pressed: Any) -> dict[str, float]:
        updates: dict[str, float] = {}
        self._state.encoder_delta = self._poll_encoder_delta(
            pressed=pressed,
            encoder_id="menu",
            left_key="menu_rotate_left",
            right_key="menu_rotate_right",
            fallback_left="rotate_left",
            fallback_right="rotate_right",
        )
        updates["inputs.rotary.menu.delta"] = self._state.encoder_delta
        if self._poll_encoder_press(pressed, "menu_press", "menu_press", "press"):
            self._state.encoder_presses += 1.0
            updates["inputs.rotary.menu.presses"] = self._state.encoder_presses

        self._state.aux_encoder_delta = self._poll_encoder_delta(
            pressed=pressed,
            encoder_id="aux",
            left_key="aux_rotate_left",
            right_key="aux_rotate_right",
            fallback_left=None,
            fallback_right=None,
        )
        updates["inputs.rotary.aux.delta"] = self._state.aux_encoder_delta
        if self._poll_encoder_press(pressed, "aux_press", "aux_press", None):
            self._state.aux_encoder_presses += 1.0
            updates["inputs.rotary.aux.presses"] = self._state.aux_encoder_presses

        speed_changed = self._update_axis(pressed, "speed_up", "speed_down", "speed", 2.0, 0.0, 140.0)
        rpm_changed = self._update_axis(pressed, "rpm_up", "rpm_down", "rpm", 120.0, 0.0, self._rpm_max)
        fuel_changed = self._update_axis(pressed, "fuel_up", "fuel_down", "fuel", 1.0, 0.0, 100.0)
        temp_changed = self._update_axis(
            pressed, "temp_up", "temp_down", "temp", 1.0, self._temp_min, self._temp_max
        )
        amp_changed = self._update_axis(
            pressed, "amp_up", "amp_down", "amp", 1.0, self._amp_min, self._amp_max
        )
        oil_changed = self._update_axis(
            pressed, "oil_up", "oil_down", "oil", 1.0, self._oil_min, self._oil_max
        )

        if speed_changed:
            updates["speed_sensor.vehicle_speed"] = self._state.speed
        if rpm_changed:
            updates["analog_inputs.engine_rpm"] = self._state.rpm
        if fuel_changed:
            updates["analog_inputs.fuel_level"] = self._state.fuel
        if temp_changed:
            updates["analog_inputs.coolant_temp"] = self._state.temp
        if amp_changed:
            updates["analog_inputs.system_amp"] = self._state.amp
        if oil_changed:
            updates["indicators.oil"] = self._state.oil

        if self._poll_encoder_press(pressed, "turn_left_toggle", "turn_left_toggle", None):
            self._state.turn_left = 0.0 if self._state.turn_left > 0.0 else 1.0
            self._state.turn_right = 0.0
            updates["indicators.turn_left"] = self._state.turn_left
            updates["indicators.turn_right"] = self._state.turn_right
        if self._poll_encoder_press(pressed, "turn_right_toggle", "turn_right_toggle", None):
            self._state.turn_right = 0.0 if self._state.turn_right > 0.0 else 1.0
            self._state.turn_left = 0.0
            updates["indicators.turn_left"] = self._state.turn_left
            updates["indicators.turn_right"] = self._state.turn_right
        if self._poll_encoder_press(pressed, "hazard_toggle", "hazard_toggle", None):
            both_on = self._state.turn_left > 0.0 and self._state.turn_right > 0.0
            next_value = 0.0 if both_on else 1.0
            self._state.turn_left = next_value
            self._state.turn_right = next_value
            updates["indicators.turn_left"] = self._state.turn_left
            updates["indicators.turn_right"] = self._state.turn_right

        return updates

    def _poll_encoder_delta(
        self,
        pressed: Any,
        encoder_id: str,
        left_key: str,
        right_key: str,
        fallback_left: str | None,
        fallback_right: str | None,
    ) -> float:
        left = self._key(left_key)
        right = self._key(right_key)
        if left is None and fallback_left:
            left = self._key(fallback_left)
        if right is None and fallback_right:
            right = self._key(fallback_right)
        pressed_left = bool(left is not None and pressed[left])
        pressed_right = bool(right is not None and pressed[right])
        if not pressed_left and not pressed_right:
            return 0.0

        now = time.monotonic()
        if (now - self._last_emit_s[encoder_id]) < self._repeat_s:
            return 0.0
        self._last_emit_s[encoder_id] = now
        delta = 0.0
        if pressed_left:
            delta -= 1.0
        if pressed_right:
            delta += 1.0
        return delta

    def _poll_encoder_press(
        self,
        pressed: Any,
        state_key: str,
        primary_key: str,
        fallback_key: str | None,
    ) -> bool:
        key = self._key(primary_key)
        if key is None and fallback_key:
            key = self._key(fallback_key)
        is_down = bool(key is not None and pressed[key])
        was_down = self._last_pressed[state_key]
        self._last_pressed[state_key] = is_down
        return is_down and not was_down

    def _update_axis(
        self,
        pressed: Any,
        up_name: str,
        down_name: str,
        attr_name: str,
        step: float,
        min_v: float,
        max_v: float,
    ) -> bool:
        value = getattr(self._state, attr_name)
        previous = value
        key_up = self._key(up_name)
        key_down = self._key(down_name)
        if key_up is not None and pressed[key_up]:
            value += step
        if key_down is not None and pressed[key_down]:
            value -= step
        value = max(min_v, min(max_v, value))
        setattr(self._state, attr_name, value)
        return value != previous
