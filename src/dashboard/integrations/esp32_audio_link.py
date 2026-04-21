from __future__ import annotations

from typing import Any

try:
    from smbus2 import SMBus  # type: ignore
except ImportError:  # pragma: no cover
    SMBus = None


def _i2c_metadata_bytes(track: str, max_len: int = 32) -> bytes:
    """SMBus block payload is capped at 32 bytes; prefer song title when 'Artist - Title' is too long."""
    raw = (track or "").strip() or "-"
    enc_full = raw.encode("ascii", errors="ignore")
    if len(enc_full) <= max_len:
        return enc_full[:max_len]
    if " - " in raw:
        _artist, title = raw.split(" - ", 1)
        title = title.strip()
        enc_title = title.encode("ascii", errors="ignore") if title else b""
        if enc_title and len(enc_title) <= max_len:
            return enc_title[:max_len]
    return enc_full[:max_len]


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
        encoded = _i2c_metadata_bytes(track, 32)
        try:
            self._bus.write_i2c_block_data(self._address, 0x01, list(encoded))
            self._status = "online"
        except Exception:
            self._status = "offline"

    def publish_transport(self, elapsed_s: int, duration_s: int, vol_0_15: int = 0) -> None:
        if self._bus is None:
            return
        el = max(0, min(0xFFFF, int(elapsed_s)))
        du = max(0, min(0xFFFF, int(duration_s)))
        vol = max(0, min(15, int(vol_0_15)))
        payload = [el & 0xFF, (el >> 8) & 0xFF, du & 0xFF, (du >> 8) & 0xFF, vol]
        try:
            self._bus.write_i2c_block_data(self._address, 0x05, payload)
            self._status = "online"
        except Exception:
            self._status = "offline"

    def status(self) -> str:
        return self._status
