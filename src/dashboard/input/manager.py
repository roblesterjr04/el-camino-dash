from __future__ import annotations

from typing import Any

import pygame

from dashboard.input.analog_adc import AnalogAdcInput
from dashboard.input.arduino_serial import ArduinoSerialInput
from dashboard.input.encoder_arduino_serial import EncoderArduinoSerialInput
from dashboard.input.gpio_buttons import GpioButtonInput
from dashboard.input.ir_remote import IrRemoteInput
from dashboard.input.keyboard_sim import KeyboardSimInput
from dashboard.input.mcp23017_controls import Mcp23017ControlsInput
from dashboard.input.rotary_encoder import RotaryEncoderInput
from dashboard.input.simulated_data import SimulatedDataInput
from dashboard.input.speed_sensor import SpeedSensorInput


class InputManager:
    def __init__(self, config: dict[str, Any]) -> None:
        input_cfg = config["inputs"]
        self._rotary = [RotaryEncoderInput(spec) for spec in input_cfg["rotary_encoders"] if spec.get("enabled", False)]
        self._gpio_buttons = GpioButtonInput(input_cfg.get("gpio_buttons", {"enabled": False}))
        self._mcp23017_controls = Mcp23017ControlsInput(input_cfg.get("mcp23017_controls", {"enabled": False}))
        self._ir_remote = IrRemoteInput(input_cfg.get("ir_remote", {"enabled": False}))
        self._analog = [AnalogAdcInput(spec) for spec in input_cfg["analog_inputs"] if spec.get("enabled", False)]
        self._speed = [SpeedSensorInput(spec) for spec in input_cfg["speed_sensors"] if spec.get("enabled", False)]
        self._arduino_serial = ArduinoSerialInput(input_cfg.get("arduino_serial", {"enabled": False}))
        self._encoder_arduino_serial = EncoderArduinoSerialInput(
            input_cfg.get("encoder_arduino_serial", {"enabled": False})
        )
        self._simulated = SimulatedDataInput(input_cfg["simulated_data"])
        self._keyboard = KeyboardSimInput(input_cfg["keyboard_sim"])
        self._keyboard_enabled = bool(input_cfg["keyboard_sim"].get("enabled", False))
        self._keyboard_active = True
        self._rotary_press_totals: dict[str, float] = {
            "inputs.rotary.menu.presses": 0.0,
            "inputs.rotary.aux.presses": 0.0,
        }
        self._rotary_press_last_by_source: dict[tuple[str, str], float] = {}

    def set_keyboard_active(self, active: bool) -> None:
        self._keyboard_active = bool(active)

    def ir_last_code(self) -> str:
        return self._ir_remote.last_code

    def encoder_last_frame(self) -> str:
        return self._encoder_arduino_serial.last_frame

    def poll(self) -> dict[str, float]:
        values: dict[str, float] = {}
        for source in self._rotary:
            values.update(source.poll())
        for source in self._analog:
            values.update(source.poll())
        for source in self._speed:
            values.update(source.poll())
        values.update(self._simulated.poll())
        values.update(self._arduino_serial.poll())

        pressed = pygame.key.get_pressed()
        if self._keyboard_enabled and self._keyboard_active:
            values.update(self._keyboard.poll(pressed))
        ir_values = self._ir_remote.poll()
        self._merge_rotary_actions(values, ir_values, source_id="ir_remote")
        self._merge_rotary_actions(values, self._encoder_arduino_serial.poll(), source_id="encoder_arduino_serial")
        self._merge_rotary_actions(values, self._mcp23017_controls.poll(), source_id="mcp23017_controls")
        for key, value in ir_values.items():
            if key.startswith("inputs.ir.command."):
                values[key] = float(value)
        self._merge_rotary_actions(values, self._gpio_buttons.poll(), source_id="gpio_buttons")
        for key, total in self._rotary_press_totals.items():
            values[key] = float(total)
        return values

    def _merge_rotary_actions(self, values: dict[str, float], additions: dict[str, float], *, source_id: str) -> None:
        if not additions:
            return
        for key in ("inputs.rotary.menu.delta", "inputs.rotary.aux.delta"):
            values[key] = float(values.get(key, 0.0)) + float(additions.get(key, 0.0))
        for key in ("inputs.rotary.menu.presses", "inputs.rotary.aux.presses"):
            current = float(additions.get(key, 0.0))
            stamp = (source_id, key)
            if stamp not in self._rotary_press_last_by_source:
                # Baseline each source counter to avoid startup jumps.
                self._rotary_press_last_by_source[stamp] = current
                values[key] = float(self._rotary_press_totals.get(key, 0.0))
                continue
            previous = float(self._rotary_press_last_by_source[stamp])
            self._rotary_press_last_by_source[stamp] = current
            if current <= previous:
                # Source counter reset/no-op: do not decrement global totals.
                values[key] = float(self._rotary_press_totals.get(key, 0.0))
                continue
            delta = current - previous
            next_total = float(self._rotary_press_totals.get(key, 0.0)) + delta
            self._rotary_press_totals[key] = next_total
            values[key] = next_total
