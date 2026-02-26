from __future__ import annotations

import time
from typing import Any

try:
    import RPi.GPIO as GPIO  # type: ignore
except ImportError:  # pragma: no cover
    GPIO = None


class SpeedSensorInput:
    def __init__(self, spec: dict[str, Any]) -> None:
        self.spec = spec
        self._last_time = time.monotonic()
        self._last_speed = 0.0
        self._pulse_count = 0
        self._ready = False
        if GPIO and spec.get("enabled", False):
            GPIO.setmode(GPIO.BCM)
            GPIO.setup(spec["gpio_pin"], GPIO.IN, pull_up_down=GPIO.PUD_UP)
            GPIO.add_event_detect(spec["gpio_pin"], GPIO.RISING, callback=self._on_pulse)
            self._ready = True

    def _on_pulse(self, _: int) -> None:
        self._pulse_count += 1

    def poll(self) -> dict[str, float]:
        now = time.monotonic()
        elapsed = max(0.001, now - self._last_time)
        ppr = float(self.spec["pulses_per_revolution"])
        circ = float(self.spec["wheel_circumference_m"])
        if self._ready:
            rev_per_s = (self._pulse_count / ppr) / elapsed
            speed_mps = rev_per_s * circ
            speed_mph = speed_mps * 2.236936
            self._pulse_count = 0
        else:
            speed_mph = 0.0
        alpha = float(self.spec.get("smoothing_alpha", 0.35))
        speed_mph = (alpha * speed_mph) + ((1.0 - alpha) * self._last_speed)
        self._last_speed = speed_mph
        self._last_time = now
        return {f"speed_sensor.{self.spec['id']}": speed_mph}
