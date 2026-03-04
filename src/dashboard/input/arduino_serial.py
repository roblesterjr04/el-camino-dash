from __future__ import annotations

from dataclasses import dataclass
from typing import Any

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    serial = None


SYNC = b"\xAA\x55"
PAYLOAD_LEN = 19
FRAME_LEN = 2 + PAYLOAD_LEN + 1


@dataclass
class _SerialRanges:
    fuel: tuple[float, float]
    temp: tuple[float, float]
    oil: tuple[float, float]
    voltage: tuple[float, float]


class ArduinoSerialInput:
    def __init__(self, spec: dict[str, Any]) -> None:
        self._spec = spec
        self._enabled = bool(spec.get("enabled", False))
        self._buffer = bytearray()
        self._port: Any | None = None
        self._wheel_circumference_m = max(0.01, float(spec.get("wheel_circumference_m", 2.1)))
        self._speed_pulses_per_rev = max(1.0, float(spec.get("speed_pulses_per_rev", 1.0)))
        self._ranges = _SerialRanges(
            fuel=self._parse_range(spec.get("fuel_range"), (0.0, 100.0)),
            temp=self._parse_range(spec.get("temp_range"), (100.0, 260.0)),
            oil=self._parse_range(spec.get("oil_range"), (0.0, 100.0)),
            voltage=self._parse_range(spec.get("voltage_range"), (10.0, 16.0)),
        )
        if not self._enabled:
            return
        if serial is None:
            self._enabled = False
            return
        port_name = str(spec.get("port", "/dev/ttyAMA0"))
        baudrate = int(spec.get("baudrate", 115200))
        timeout_s = float(spec.get("timeout_s", 0.0))
        try:
            self._port = serial.Serial(port_name, baudrate=baudrate, timeout=timeout_s)
        except Exception:
            self._port = None
            self._enabled = False

    def _parse_range(self, raw: Any, fallback: tuple[float, float]) -> tuple[float, float]:
        if not isinstance(raw, list) or len(raw) != 2:
            return fallback
        lo = float(raw[0])
        hi = float(raw[1])
        if hi <= lo:
            return fallback
        return (lo, hi)

    def _map_adc(self, raw: int, out_range: tuple[float, float]) -> float:
        ratio = max(0.0, min(1.0, float(raw) / 1023.0))
        return out_range[0] + ((out_range[1] - out_range[0]) * ratio)

    def _parse_frame(self, payload: bytes) -> dict[str, float]:
        pot = int.from_bytes(payload[0:2], "big")
        therm = int.from_bytes(payload[2:4], "big")
        pressure = int.from_bytes(payload[4:6], "big")
        voltage = int.from_bytes(payload[6:8], "big")
        speed_rpm = int.from_bytes(payload[8:12], "big")
        engine_rpm = int.from_bytes(payload[12:16], "big")
        flags = int(payload[16])
        faults = int.from_bytes(payload[17:19], "big")

        wheel_rps = (float(speed_rpm) / self._speed_pulses_per_rev) / 60.0
        speed_mps = wheel_rps * self._wheel_circumference_m
        speed_mph = speed_mps * 2.236936

        return {
            "speed_sensor.vehicle_speed": speed_mph,
            "analog_inputs.engine_rpm": float(engine_rpm),
            "analog_inputs.fuel_level": self._map_adc(pot, self._ranges.fuel),
            "analog_inputs.coolant_temp": self._map_adc(therm, self._ranges.temp),
            "analog_inputs.oil_pressure": self._map_adc(pressure, self._ranges.oil),
            "indicators.oil": self._map_adc(pressure, self._ranges.oil),
            "analog_inputs.system_voltage": self._map_adc(voltage, self._ranges.voltage),
            "indicators.alt_ok": 1.0 if (flags & 0x01) else 0.0,
            "indicators.ign_on": 1.0 if (flags & 0x02) else 0.0,
            "arduino.fault_word": float(faults),
        }

    def _extract_latest_packet(self) -> dict[str, float]:
        if self._port is None:
            return {}
        try:
            chunk = self._port.read(256)
        except Exception:
            return {}
        if chunk:
            self._buffer.extend(chunk)
        latest: dict[str, float] = {}
        while len(self._buffer) >= FRAME_LEN:
            sync_idx = self._buffer.find(SYNC)
            if sync_idx < 0:
                self._buffer.clear()
                break
            if sync_idx > 0:
                del self._buffer[:sync_idx]
            if len(self._buffer) < FRAME_LEN:
                break
            payload = bytes(self._buffer[2 : 2 + PAYLOAD_LEN])
            recv_crc = self._buffer[2 + PAYLOAD_LEN]
            calc_crc = 0
            for byte in payload:
                calc_crc ^= byte
            del self._buffer[:FRAME_LEN]
            if recv_crc != calc_crc:
                continue
            latest = self._parse_frame(payload)
        return latest

    def poll(self) -> dict[str, float]:
        if not self._enabled:
            return {}
        return self._extract_latest_packet()
