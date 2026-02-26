from __future__ import annotations

from typing import Any

import pygame

from dashboard.input.analog_adc import AnalogAdcInput
from dashboard.input.keyboard_sim import KeyboardSimInput
from dashboard.input.rotary_encoder import RotaryEncoderInput
from dashboard.input.simulated_data import SimulatedDataInput
from dashboard.input.speed_sensor import SpeedSensorInput


class InputManager:
    def __init__(self, config: dict[str, Any]) -> None:
        input_cfg = config["inputs"]
        self._rotary = [RotaryEncoderInput(spec) for spec in input_cfg["rotary_encoders"] if spec.get("enabled", False)]
        self._analog = [AnalogAdcInput(spec) for spec in input_cfg["analog_inputs"] if spec.get("enabled", False)]
        self._speed = [SpeedSensorInput(spec) for spec in input_cfg["speed_sensors"] if spec.get("enabled", False)]
        self._simulated = SimulatedDataInput(input_cfg["simulated_data"])
        self._keyboard = KeyboardSimInput(input_cfg["keyboard_sim"])
        self._keyboard_enabled = bool(input_cfg["keyboard_sim"].get("enabled", False))

    def poll(self) -> dict[str, float]:
        values: dict[str, float] = {}
        for source in self._rotary:
            values.update(source.poll())
        for source in self._analog:
            values.update(source.poll())
        for source in self._speed:
            values.update(source.poll())
        values.update(self._simulated.poll())

        pressed = pygame.key.get_pressed()
        if self._keyboard_enabled:
            values.update(self._keyboard.poll(pressed))

        return values
