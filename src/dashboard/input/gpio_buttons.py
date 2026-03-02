from __future__ import annotations

from dataclasses import dataclass
import time
from typing import Any

try:
    import RPi.GPIO as GPIO  # type: ignore
except ImportError:  # pragma: no cover
    GPIO = None


@dataclass
class GpioButtonState:
    last_left: int = 1
    last_select: int = 1
    presses: float = 0.0
    last_left_edge_s: float = 0.0
    last_select_edge_s: float = 0.0


class GpioButtonInput:
    """Maps simple GPIO buttons to menu rotary left/select actions."""

    def __init__(self, spec: dict[str, Any]) -> None:
        self.spec = spec
        self.state = GpioButtonState()
        self._ready = False
        self._left_pin = int(spec.get("menu_left_pin", -1))
        self._select_pin = int(spec.get("menu_select_pin", -1))
        self._pull_up = bool(spec.get("pull_up", True))
        self._left_delta = float(spec.get("left_delta", -1.0))
        self._debounce_s = max(0.0, float(spec.get("debounce_ms", 60.0)) / 1000.0)

        if not bool(spec.get("enabled", False)):
            return
        if GPIO is None:
            return
        if self._left_pin < 0 or self._select_pin < 0:
            return

        GPIO.setmode(GPIO.BCM)
        pud = GPIO.PUD_UP if self._pull_up else GPIO.PUD_DOWN
        GPIO.setup(self._left_pin, GPIO.IN, pull_up_down=pud)
        GPIO.setup(self._select_pin, GPIO.IN, pull_up_down=pud)
        self.state.last_left = GPIO.input(self._left_pin)
        self.state.last_select = GPIO.input(self._select_pin)
        self._ready = True

    def poll(self) -> dict[str, float]:
        if not self._ready:
            return {"inputs.rotary.menu.delta": 0.0, "inputs.rotary.menu.presses": 0.0}

        assert GPIO is not None
        now_s = time.monotonic()
        left_now = GPIO.input(self._left_pin)
        select_now = GPIO.input(self._select_pin)

        delta = 0.0
        if left_now == 0 and self.state.last_left != 0:
            if (now_s - self.state.last_left_edge_s) >= self._debounce_s:
                delta = self._left_delta
                self.state.last_left_edge_s = now_s
        self.state.last_left = left_now

        if select_now == 0 and self.state.last_select != 0:
            if (now_s - self.state.last_select_edge_s) >= self._debounce_s:
                self.state.presses += 1.0
                self.state.last_select_edge_s = now_s
        self.state.last_select = select_now

        return {
            "inputs.rotary.menu.delta": delta,
            "inputs.rotary.menu.presses": self.state.presses,
        }
