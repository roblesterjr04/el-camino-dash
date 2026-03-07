from __future__ import annotations

from dataclasses import dataclass
import time
from typing import Any

try:
    import smbus2  # type: ignore
except ImportError:  # pragma: no cover
    smbus2 = None


# MCP23017 register map.
IODIRA = 0x00
IODIRB = 0x01
GPPUA = 0x0C
GPPUB = 0x0D
GPIOA = 0x12
GPIOB = 0x13


@dataclass
class McpControlState:
    last_menu_a: int = 1
    last_aux_a: int = 1
    last_menu_button: int = 1
    last_aux_button: int = 1
    last_menu_left: int = 1
    last_menu_select: int = 1
    menu_presses: float = 0.0
    aux_presses: float = 0.0
    last_menu_press_s: float = 0.0
    last_aux_press_s: float = 0.0
    last_menu_left_edge_s: float = 0.0
    last_menu_select_edge_s: float = 0.0
    last_menu_rotary_edge_s: float = 0.0
    last_aux_rotary_edge_s: float = 0.0
    last_menu_ab: int = 0
    last_aux_ab: int = 0
    menu_quad_accum: int = 0
    aux_quad_accum: int = 0


class Mcp23017ControlsInput:
    """Maps MCP23017 GPIO inputs to rotary-style dashboard actions."""

    def __init__(self, spec: dict[str, Any]) -> None:
        self._spec = spec
        self._ready = False
        self._state = McpControlState()
        self._bus = None
        self._address = int(spec.get("address", 0x20))
        self._pull_up = bool(spec.get("pull_up", True))
        self._edge_debounce_s = max(0.0, float(spec.get("debounce_ms", 60.0)) / 1000.0)
        self._rotary_debounce_s = max(0.0, float(spec.get("rotary_debounce_ms", 4.0)) / 1000.0)
        self._button_debounce_s = max(0.0, float(spec.get("button_debounce_ms", 120.0)) / 1000.0)
        self._left_delta = float(spec.get("left_delta", -1.0))
        self._menu_accel = float(spec.get("menu_acceleration", 1.0))
        self._aux_accel = float(spec.get("aux_acceleration", 1.0))
        self._counts_per_step = max(1, int(spec.get("rotary_counts_per_step", 2)))
        self._invert_menu = bool(spec.get("invert_menu_direction", False))
        self._invert_aux = bool(spec.get("invert_aux_direction", False))

        self._pin_menu_a = int(spec.get("menu_pin_a", 0))
        self._pin_menu_b = int(spec.get("menu_pin_b", 1))
        self._pin_menu_button = int(spec.get("menu_pin_button", 2))
        self._pin_aux_a = int(spec.get("aux_pin_a", 3))
        self._pin_aux_b = int(spec.get("aux_pin_b", 4))
        self._pin_aux_button = int(spec.get("aux_pin_button", 5))
        self._pin_menu_left = int(spec.get("menu_left_pin", 6))
        self._pin_menu_select = int(spec.get("menu_select_pin", 7))

        if not bool(spec.get("enabled", False)):
            return
        if smbus2 is None:
            return

        used_pins = {
            self._pin_menu_a,
            self._pin_menu_b,
            self._pin_menu_button,
            self._pin_aux_a,
            self._pin_aux_b,
            self._pin_aux_button,
            self._pin_menu_left,
            self._pin_menu_select,
        }
        if any(pin < 0 or pin > 15 for pin in used_pins):
            return

        try:
            self._bus = smbus2.SMBus(int(spec.get("i2c_bus", 1)))
            # Make all expander lines inputs to avoid driving external circuits.
            self._write_reg(IODIRA, 0xFF)
            self._write_reg(IODIRB, 0xFF)
            if self._pull_up:
                gppu_a = self._read_reg(GPPUA)
                gppu_b = self._read_reg(GPPUB)
                for pin in used_pins:
                    if pin < 8:
                        gppu_a |= (1 << pin)
                    else:
                        gppu_b |= (1 << (pin - 8))
                self._write_reg(GPPUA, gppu_a)
                self._write_reg(GPPUB, gppu_b)

            sample = self._read_gpio_word()
            self._state.last_menu_a = self._pin_level(sample, self._pin_menu_a)
            menu_b = self._pin_level(sample, self._pin_menu_b)
            self._state.last_aux_a = self._pin_level(sample, self._pin_aux_a)
            aux_b = self._pin_level(sample, self._pin_aux_b)
            self._state.last_menu_button = self._pin_level(sample, self._pin_menu_button)
            self._state.last_aux_button = self._pin_level(sample, self._pin_aux_button)
            self._state.last_menu_left = self._pin_level(sample, self._pin_menu_left)
            self._state.last_menu_select = self._pin_level(sample, self._pin_menu_select)
            self._state.last_menu_ab = ((self._state.last_menu_a & 0x01) << 1) | (menu_b & 0x01)
            self._state.last_aux_ab = ((self._state.last_aux_a & 0x01) << 1) | (aux_b & 0x01)
            self._ready = True
        except Exception:
            self._ready = False

    def _write_reg(self, reg: int, value: int) -> None:
        assert self._bus is not None
        self._bus.write_byte_data(self._address, reg, value & 0xFF)

    def _read_reg(self, reg: int) -> int:
        assert self._bus is not None
        return int(self._bus.read_byte_data(self._address, reg)) & 0xFF

    def _read_gpio_word(self) -> int:
        gpio_a = self._read_reg(GPIOA)
        gpio_b = self._read_reg(GPIOB)
        return gpio_a | (gpio_b << 8)

    @staticmethod
    def _pin_level(sample: int, pin: int) -> int:
        return 1 if ((sample >> pin) & 0x01) else 0

    @staticmethod
    def _quadrature_delta(prev_ab: int, curr_ab: int) -> int:
        transition = (prev_ab & 0x03, curr_ab & 0x03)
        if transition in ((0, 1), (1, 3), (3, 2), (2, 0)):
            return 1
        if transition in ((0, 2), (2, 3), (3, 1), (1, 0)):
            return -1
        return 0

    def poll(self) -> dict[str, float]:
        if not self._ready:
            return {
                "inputs.rotary.menu.delta": 0.0,
                "inputs.rotary.menu.presses": 0.0,
                "inputs.rotary.aux.delta": 0.0,
                "inputs.rotary.aux.presses": 0.0,
            }
        try:
            sample = self._read_gpio_word()
        except Exception:
            return {
                "inputs.rotary.menu.delta": 0.0,
                "inputs.rotary.menu.presses": self._state.menu_presses,
                "inputs.rotary.aux.delta": 0.0,
                "inputs.rotary.aux.presses": self._state.aux_presses,
            }

        now_s = time.monotonic()
        menu_a = self._pin_level(sample, self._pin_menu_a)
        menu_b = self._pin_level(sample, self._pin_menu_b)
        menu_button = self._pin_level(sample, self._pin_menu_button)
        aux_a = self._pin_level(sample, self._pin_aux_a)
        aux_b = self._pin_level(sample, self._pin_aux_b)
        aux_button = self._pin_level(sample, self._pin_aux_button)
        menu_left = self._pin_level(sample, self._pin_menu_left)
        menu_select = self._pin_level(sample, self._pin_menu_select)

        menu_delta = 0.0
        aux_delta = 0.0

        menu_ab = ((menu_a & 0x01) << 1) | (menu_b & 0x01)
        if menu_ab != self._state.last_menu_ab:
            if (now_s - self._state.last_menu_rotary_edge_s) >= self._rotary_debounce_s:
                qd = self._quadrature_delta(self._state.last_menu_ab, menu_ab)
                if qd == 0:
                    self._state.menu_quad_accum = 0
                else:
                    if self._invert_menu:
                        qd *= -1
                    self._state.menu_quad_accum += qd
                    while self._state.menu_quad_accum >= self._counts_per_step:
                        menu_delta += self._menu_accel
                        self._state.menu_quad_accum -= self._counts_per_step
                    while self._state.menu_quad_accum <= -self._counts_per_step:
                        menu_delta -= self._menu_accel
                        self._state.menu_quad_accum += self._counts_per_step
                self._state.last_menu_rotary_edge_s = now_s
            self._state.last_menu_ab = menu_ab
        self._state.last_menu_a = menu_a

        aux_ab = ((aux_a & 0x01) << 1) | (aux_b & 0x01)
        if aux_ab != self._state.last_aux_ab:
            if (now_s - self._state.last_aux_rotary_edge_s) >= self._rotary_debounce_s:
                qd = self._quadrature_delta(self._state.last_aux_ab, aux_ab)
                if qd == 0:
                    self._state.aux_quad_accum = 0
                else:
                    if self._invert_aux:
                        qd *= -1
                    self._state.aux_quad_accum += qd
                    while self._state.aux_quad_accum >= self._counts_per_step:
                        aux_delta += self._aux_accel
                        self._state.aux_quad_accum -= self._counts_per_step
                    while self._state.aux_quad_accum <= -self._counts_per_step:
                        aux_delta -= self._aux_accel
                        self._state.aux_quad_accum += self._counts_per_step
                self._state.last_aux_rotary_edge_s = now_s
            self._state.last_aux_ab = aux_ab
        self._state.last_aux_a = aux_a

        if menu_button == 0 and self._state.last_menu_button != 0:
            if (now_s - self._state.last_menu_press_s) >= self._button_debounce_s:
                self._state.menu_presses += 1.0
                self._state.last_menu_press_s = now_s
        self._state.last_menu_button = menu_button

        if aux_button == 0 and self._state.last_aux_button != 0:
            if (now_s - self._state.last_aux_press_s) >= self._button_debounce_s:
                self._state.aux_presses += 1.0
                self._state.last_aux_press_s = now_s
        self._state.last_aux_button = aux_button

        if menu_left == 0 and self._state.last_menu_left != 0:
            if (now_s - self._state.last_menu_left_edge_s) >= self._edge_debounce_s:
                menu_delta += self._left_delta
                self._state.last_menu_left_edge_s = now_s
        self._state.last_menu_left = menu_left

        if menu_select == 0 and self._state.last_menu_select != 0:
            if (now_s - self._state.last_menu_select_edge_s) >= self._edge_debounce_s:
                self._state.menu_presses += 1.0
                self._state.last_menu_select_edge_s = now_s
        self._state.last_menu_select = menu_select

        return {
            "inputs.rotary.menu.delta": float(menu_delta),
            "inputs.rotary.menu.presses": float(self._state.menu_presses),
            "inputs.rotary.aux.delta": float(aux_delta),
            "inputs.rotary.aux.presses": float(self._state.aux_presses),
        }
