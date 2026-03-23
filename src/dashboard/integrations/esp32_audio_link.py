from __future__ import annotations

from typing import Any

try:
    from smbus2 import SMBus  # type: ignore
except ImportError:  # pragma: no cover
    SMBus = None


class Esp32AudioLink:
    """Lightweight ESP32 control/status bridge over I2C."""

    def __init__(self, spec: dict[str, Any]) -> None:
        self._enabled = bool(spec.get("enabled", False))
        self._bus_id = int(spec.get("i2c_bus", 1))
        self._address = int(spec.get("address", 0x2A))
        self._bus: SMBus | None = None
        self._status = "disabled"

    def start(self) -> None:
        if not self._enabled:
            self._status = "disabled"
            return
        if SMBus is None:
            self._status = "smbus-missing"
            return
        try:
            self._bus = SMBus(self._bus_id)
            self._status = "online"
        except Exception:
            self._bus = None
            self._status = "offline"

    def stop(self) -> None:
        if self._bus is not None:
            try:
                self._bus.close()
            except Exception:
                pass
        self._bus = None

    def publish_metadata(self, track: str) -> None:
        if self._bus is None:
            return
        encoded = (track or "-")[:32].encode("ascii", errors="ignore")
        try:
            self._bus.write_i2c_block_data(self._address, 0x01, list(encoded))
            self._status = "online"
        except Exception:
            self._status = "offline"

    def status(self) -> str:
        return self._status
