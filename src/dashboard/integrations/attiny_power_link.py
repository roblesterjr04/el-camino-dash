from __future__ import annotations

from typing import Any

try:
    from gpiozero import DigitalInputDevice, DigitalOutputDevice  # type: ignore
except ImportError:  # pragma: no cover
    DigitalInputDevice = None
    DigitalOutputDevice = None


class AttinyPowerLink:
    """GPIO handshake with ATtiny84 for safe shutdown/power state."""

    def __init__(self, spec: dict[str, Any]) -> None:
        self._enabled = bool(spec.get("enabled", False))
        self._request_pin = int(spec.get("request_pin", 16))
        self._confirm_pin = int(spec.get("confirm_pin", 12))
        self._pull_up = bool(spec.get("pull_up", True))
        self._request_in: Any | None = None
        self._confirm_out: Any | None = None
        self._status = "disabled"

    def start(self) -> None:
        if not self._enabled:
            self._status = "disabled"
            return
        if DigitalInputDevice is None or DigitalOutputDevice is None:
            self._status = "gpiozero-missing"
            return
        try:
            self._request_in = DigitalInputDevice(self._request_pin, pull_up=self._pull_up)
            self._confirm_out = DigitalOutputDevice(self._confirm_pin, initial_value=False)
            self._status = "online"
        except Exception:
            self._request_in = None
            self._confirm_out = None
            self._status = "offline"

    def stop(self) -> None:
        for dev in (self._request_in, self._confirm_out):
            if dev is None:
                continue
            try:
                dev.close()
            except Exception:
                pass
        self._request_in = None
        self._confirm_out = None

    def poll(self) -> dict[str, float]:
        if self._request_in is None or self._confirm_out is None:
            return {}
        active = bool(self._request_in.value)
        try:
            self._confirm_out.value = active
        except Exception:
            pass
        return {"power.shutdown_request": 1.0 if active else 0.0}

    def status(self) -> str:
        return self._status
