from __future__ import annotations

from dataclasses import dataclass
from typing import Any

try:
    import RPi.GPIO as GPIO  # type: ignore
except ImportError:  # pragma: no cover
    GPIO = None


@dataclass
class RotaryState:
    last_a: int = 0
    delta: float = 0.0
    presses: float = 0.0


class RotaryEncoderInput:
    def __init__(self, spec: dict[str, Any]) -> None:
        self.spec = spec
        self.state = RotaryState()
        self._ready = False
        if GPIO and spec.get("enabled", False):
            GPIO.setmode(GPIO.BCM)
            GPIO.setup(spec["pin_a"], GPIO.IN, pull_up_down=GPIO.PUD_UP if spec.get("pull_up", True) else GPIO.PUD_DOWN)
            GPIO.setup(spec["pin_b"], GPIO.IN, pull_up_down=GPIO.PUD_UP if spec.get("pull_up", True) else GPIO.PUD_DOWN)
            GPIO.setup(
                spec["pin_button"],
                GPIO.IN,
                pull_up_down=GPIO.PUD_UP if spec.get("pull_up", True) else GPIO.PUD_DOWN,
            )
            self.state.last_a = GPIO.input(spec["pin_a"])
            self._ready = True

    def poll(self) -> dict[str, float]:
        if not self._ready:
            return {f"inputs.rotary.{self.spec['id']}.delta": 0.0, f"inputs.rotary.{self.spec['id']}.presses": 0.0}

        a = GPIO.input(self.spec["pin_a"])
        b = GPIO.input(self.spec["pin_b"])
        button = GPIO.input(self.spec["pin_button"])

        delta = 0.0
        if a != self.state.last_a:
            direction = -1.0 if b != a else 1.0
            if self.spec.get("invert_direction", False):
                direction *= -1.0
            delta += direction * float(self.spec.get("acceleration", 1.0))
        self.state.last_a = a
        self.state.delta = delta
        if button == 0:
            self.state.presses += 1.0

        return {
            f"inputs.rotary.{self.spec['id']}.delta": self.state.delta,
            f"inputs.rotary.{self.spec['id']}.presses": self.state.presses,
        }
