from __future__ import annotations

from typing import Any

try:
    from smbus2 import SMBus  # type: ignore
except ImportError:  # pragma: no cover
    SMBus = None


class SensorPackI2CInput:
    """Reads a fixed sensor frame from the ATmega328 over I2C."""

    def __init__(self, spec: dict[str, Any]) -> None:
        self._enabled = bool(spec.get("enabled", False))
        self._bus_id = int(spec.get("bus", 1))
        self._address = int(spec.get("address", 0x12))
        self._register = int(spec.get("register", 0x00))
        self._frame_len = max(12, int(spec.get("frame_len", 16)))
        self._bus: SMBus | None = None
        if not self._enabled or SMBus is None:
            self._enabled = False
            return
        try:
            self._bus = SMBus(self._bus_id)
        except Exception:
            self._bus = None
            self._enabled = False

    def is_enabled(self) -> bool:
        return bool(self._enabled)

    def _read_frame(self) -> bytes:
        if self._bus is None:
            return b""
        try:
            payload = self._bus.read_i2c_block_data(self._address, self._register, self._frame_len)
        except Exception:
            return b""
        return bytes(payload)

    def poll(self) -> dict[str, float]:
        if not self._enabled:
            return {}
        frame = self._read_frame()
        if len(frame) < 16:
            return {}
        # Frame map (big endian):
        # [0..1] speed*10 mph, [2..3] rpm, [4..5] fuel*10,
        # [6..7] engine coolant temp *10 F, [8..9] oil*10 psi, [10..11] voltage*10 V,
        # (no outside/ambient channel in this 16-byte frame)
        # [12] flags, [13..14] fault_word, [15] reserved/crc.
        speed_tenths = int.from_bytes(frame[0:2], "big", signed=False)
        rpm = int.from_bytes(frame[2:4], "big", signed=False)
        fuel_tenths = int.from_bytes(frame[4:6], "big", signed=False)
        temp_tenths = int.from_bytes(frame[6:8], "big", signed=False)
        oil_tenths = int.from_bytes(frame[8:10], "big", signed=False)
        volt_tenths = int.from_bytes(frame[10:12], "big", signed=False)
        flags = int(frame[12])
        faults = int.from_bytes(frame[13:15], "big", signed=False)
        return {
            "speed_sensor.vehicle_speed": float(speed_tenths) / 10.0,
            "analog_inputs.engine_rpm": float(rpm),
            "analog_inputs.fuel_level": float(fuel_tenths) / 10.0,
            "analog_inputs.coolant_temp": float(temp_tenths) / 10.0,
            "analog_inputs.oil_pressure": float(oil_tenths) / 10.0,
            "indicators.oil": float(oil_tenths) / 10.0,
            "analog_inputs.system_voltage": float(volt_tenths) / 10.0,
            "indicators.alt_ok": 1.0 if (flags & 0x01) else 0.0,
            "indicators.ign_on": 1.0 if (flags & 0x02) else 0.0,
            "arduino.fault_word": float(faults),
        }
