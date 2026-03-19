from __future__ import annotations

from dataclasses import dataclass
import os
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
        self._enabled = bool(spec.get("enabled", False))
        self._ready = False
        self._state = McpControlState()
        self._bus = None
        self._log_path = self._boot_log_path("elc-i2c-input.log")
        self._last_i2c_error_log_s = 0.0
        self._last_i2c_activity_log_s = 0.0
        self._last_open_attempt_s = 0.0
        self._open_retry_interval_s = max(0.5, float(spec.get("reopen_interval_s", 2.0)))
        self._i2c_bus = int(spec.get("i2c_bus", 1))
        self._address = int(spec.get("address", 0x20))
        self._pull_up = bool(spec.get("pull_up", True))
        self._edge_debounce_s = max(0.0, float(spec.get("debounce_ms", 60.0)) / 1000.0)
        self._rotary_debounce_s = max(0.0, float(spec.get("rotary_debounce_ms", 4.0)) / 1000.0)
        self._button_debounce_s = max(0.0, float(spec.get("button_debounce_ms", 120.0)) / 1000.0)
        self._left_delta = float(spec.get("left_delta", -1.0))
        self._menu_accel = float(spec.get("menu_acceleration", 1.0))
        self._aux_accel = float(spec.get("aux_acceleration", 1.0))
        self._counts_per_step = max(1, int(spec.get("rotary_counts_per_step", 2)))
        self._i2c_read_retries = max(1, int(spec.get("i2c_read_retries", 5)))
        self._i2c_retry_delay_s = max(0.0, float(spec.get("i2c_retry_delay_ms", 1.0)) / 1000.0)
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

        self._used_pins = {
            self._pin_menu_a,
            self._pin_menu_b,
            self._pin_menu_button,
            self._pin_aux_a,
            self._pin_aux_b,
            self._pin_aux_button,
            self._pin_menu_left,
            self._pin_menu_select,
        }

        if not self._enabled:
            return
        if smbus2 is None:
            self._append_log("mcp23017 disabled: smbus2 unavailable")
            return

        if any(pin < 0 or pin > 15 for pin in self._used_pins):
            self._append_log("mcp23017 disabled: invalid pin configuration")
            return

        self._try_open(force=True)

    def _drop_bus(self) -> None:
        if self._bus is None:
            return
        try:
            self._bus.close()
        except Exception:
            pass
        self._bus = None

    def _try_open(self, *, force: bool) -> None:
        if self._ready or not self._enabled or smbus2 is None:
            return
        now = time.monotonic()
        if not force and (now - self._last_open_attempt_s) < self._open_retry_interval_s:
            return
        self._last_open_attempt_s = now
        self._drop_bus()
        try:
            self._bus = smbus2.SMBus(self._i2c_bus)
            # Make all expander lines inputs to avoid driving external circuits.
            self._write_reg(IODIRA, 0xFF)
            self._write_reg(IODIRB, 0xFF)
            if self._pull_up:
                gppu_a = self._read_reg(GPPUA)
                gppu_b = self._read_reg(GPPUB)
                for pin in self._used_pins:
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
            self._append_log(
                f"mcp23017 ready: bus={self._i2c_bus} addr=0x{self._address:02X} pull_up={int(self._pull_up)}"
            )
        except Exception as exc:
            self._ready = False
            self._drop_bus()
            if (now - self._last_i2c_error_log_s) >= 5.0:
                self._append_log(f"mcp23017 init failed: {exc}")
                self._last_i2c_error_log_s = now

    @staticmethod
    def _boot_log_path(filename: str) -> str:
        base = "/boot/firmware" if os.path.isdir("/boot/firmware") else "/boot"
        return f"{base}/{filename}"

    def _append_log(self, message: str) -> None:
        line = f"[i2c-input] {message}"
        print(line, flush=True)
        try:
            ts = time.strftime("%Y-%m-%dT%H:%M:%S%z")
            with open(self._log_path, "a", encoding="utf-8") as handle:
                handle.write(f"[{ts}] {message}\n")
        except OSError:
            pass

    def _write_reg(self, reg: int, value: int) -> None:
        assert self._bus is not None
        self._bus.write_byte_data(self._address, reg, value & 0xFF)

    def _read_reg(self, reg: int) -> int:
        assert self._bus is not None
        return int(self._bus.read_byte_data(self._address, reg)) & 0xFF

    def _read_gpio_word(self) -> int:
        assert self._bus is not None
        last_error: Exception | None = None
        for _ in range(self._i2c_read_retries):
            try:
                # Prefer a contiguous two-byte read to reduce bus transactions.
                block = self._bus.read_i2c_block_data(self._address, GPIOA, 2)
                if len(block) >= 2:
                    gpio_a = int(block[0]) & 0xFF
                    gpio_b = int(block[1]) & 0xFF
                    return gpio_a | (gpio_b << 8)
                gpio_a = self._read_reg(GPIOA)
                gpio_b = self._read_reg(GPIOB)
                return gpio_a | (gpio_b << 8)
            except Exception as exc:
                last_error = exc
                if self._i2c_retry_delay_s > 0.0:
                    time.sleep(self._i2c_retry_delay_s)
        if last_error is not None:
            raise last_error
        raise RuntimeError("MCP23017 GPIO read failed without exception")

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
            self._try_open(force=False)
            return {
                "inputs.rotary.menu.delta": 0.0,
                "inputs.rotary.menu.presses": self._state.menu_presses,
                "inputs.rotary.aux.delta": 0.0,
                "inputs.rotary.aux.presses": self._state.aux_presses,
            }
        try:
            sample = self._read_gpio_word()
        except Exception as exc:
            now_s = time.monotonic()
            if (now_s - self._last_i2c_error_log_s) >= 5.0:
                self._append_log(f"mcp23017 read error: {exc}")
                self._last_i2c_error_log_s = now_s
            self._ready = False
            self._drop_bus()
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

        menu_press_edge = menu_button == 0 and self._state.last_menu_button != 0
        if menu_press_edge:
            if (now_s - self._state.last_menu_press_s) >= self._button_debounce_s:
                self._state.menu_presses += 1.0
                self._state.last_menu_press_s = now_s
        self._state.last_menu_button = menu_button

        aux_press_edge = aux_button == 0 and self._state.last_aux_button != 0
        if aux_press_edge:
            if (now_s - self._state.last_aux_press_s) >= self._button_debounce_s:
                self._state.aux_presses += 1.0
                self._state.last_aux_press_s = now_s
        self._state.last_aux_button = aux_button

        if menu_left == 0 and self._state.last_menu_left != 0:
            if (now_s - self._state.last_menu_left_edge_s) >= self._edge_debounce_s:
                menu_delta += self._left_delta
                self._state.last_menu_left_edge_s = now_s
        self._state.last_menu_left = menu_left

        menu_select_edge = menu_select == 0 and self._state.last_menu_select != 0
        if menu_select_edge:
            if (now_s - self._state.last_menu_select_edge_s) >= self._edge_debounce_s:
                self._state.menu_presses += 1.0
                self._state.last_menu_select_edge_s = now_s
        self._state.last_menu_select = menu_select

        if (
            menu_delta != 0.0
            or aux_delta != 0.0
            or menu_press_edge
            or aux_press_edge
            or menu_select_edge
        ):
            if (now_s - self._last_i2c_activity_log_s) >= 0.05:
                self._append_log(
                    "mcp23017 activity: "
                    f"menu_delta={menu_delta:+.1f} aux_delta={aux_delta:+.1f} "
                    f"menu_presses={self._state.menu_presses:.0f} aux_presses={self._state.aux_presses:.0f}"
                )
                self._last_i2c_activity_log_s = now_s

        return {
            "inputs.rotary.menu.delta": float(menu_delta),
            "inputs.rotary.menu.presses": float(self._state.menu_presses),
            "inputs.rotary.aux.delta": float(aux_delta),
            "inputs.rotary.aux.presses": float(self._state.aux_presses),
        }
