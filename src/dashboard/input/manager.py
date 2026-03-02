from __future__ import annotations

from typing import Any

import pygame

from dashboard.input.analog_adc import AnalogAdcInput
from dashboard.input.gpio_buttons import GpioButtonInput
from dashboard.input.keyboard_sim import KeyboardSimInput
from dashboard.input.rotary_encoder import RotaryEncoderInput
from dashboard.input.simulated_data import SimulatedDataInput
from dashboard.input.speed_sensor import SpeedSensorInput


class InputManager:
    def __init__(self, config: dict[str, Any]) -> None:
        input_cfg = config["inputs"]
        self._rotary = [RotaryEncoderInput(spec) for spec in input_cfg["rotary_encoders"] if spec.get("enabled", False)]
        self._gpio_buttons = GpioButtonInput(input_cfg.get("gpio_buttons", {"enabled": False}))
        self._analog = [AnalogAdcInput(spec) for spec in input_cfg["analog_inputs"] if spec.get("enabled", False)]
        self._speed = [SpeedSensorInput(spec) for spec in input_cfg["speed_sensors"] if spec.get("enabled", False)]
        self._simulated = SimulatedDataInput(input_cfg["simulated_data"])
        self._keyboard = KeyboardSimInput(input_cfg["keyboard_sim"])
        self._keyboard_enabled = bool(input_cfg["keyboard_sim"].get("enabled", False))
        self._keyboard_active = True

    def set_keyboard_active(self, active: bool) -> None:
        self._keyboard_active = bool(active)

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
        if self._keyboard_enabled and self._keyboard_active:
            values.update(self._keyboard.poll(pressed))
        gpio_menu = self._gpio_buttons.poll()
        if gpio_menu:
            values["inputs.rotary.menu.delta"] = float(values.get("inputs.rotary.menu.delta", 0.0)) + float(
                gpio_menu.get("inputs.rotary.menu.delta", 0.0)
            )
            rotary_presses = float(values.get("inputs.rotary.menu.presses", 0.0))
            gpio_presses = float(gpio_menu.get("inputs.rotary.menu.presses", 0.0))
            values["inputs.rotary.menu.presses"] = max(rotary_presses, gpio_presses)

        return values
