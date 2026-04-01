from __future__ import annotations

from dataclasses import dataclass
import math
import os
import time
from typing import Any

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    serial = None


SYNC = b"\xAA\x55"
LEGACY_PAYLOAD_LEN = 19
EXTENDED_PAYLOAD_LEN = 21
PI_HANDSHAKE_BYTE = b"\xA5"
PI_HANDSHAKE_INTERVAL_S = 0.25


@dataclass
class _SerialRanges:
    fuel: tuple[float, float]
    temp: tuple[float, float]
    ambient_temp: tuple[float, float]
    oil: tuple[float, float]
    voltage: tuple[float, float]
    fuel_raw: tuple[float, float]
    temp_raw: tuple[float, float]
    ambient_temp_raw: tuple[float, float]
    oil_raw: tuple[float, float]
    voltage_raw: tuple[float, float]


class ArduinoSerialInput:
    def __init__(self, spec: dict[str, Any]) -> None:
        self._spec = spec
        self._enabled = bool(spec.get("enabled", False))
        self._buffer = bytearray()
        self._port: Any | None = None
        self._baudrate = int(spec.get("baudrate", 115200))
        self._timeout_s = float(spec.get("timeout_s", 0.0))
        self._port_candidates: list[str] = []
        self._last_open_attempt_monotonic = 0.0
        self._open_retry_interval_s = 1.0
        self._last_handshake_sent_monotonic = 0.0
        self._wheel_circumference_m = max(0.01, float(spec.get("wheel_circumference_m", 2.1)))
        self._speed_pulses_per_rev = max(1.0, float(spec.get("speed_pulses_per_rev", 1.0)))
        self._ranges = _SerialRanges(
            fuel=self._parse_range(spec.get("fuel_range"), (0.0, 100.0)),
            temp=self._parse_range(spec.get("temp_range"), (100.0, 260.0)),
            ambient_temp=self._parse_range(spec.get("ambient_temp_range"), (-20.0, 120.0)),
            oil=self._parse_range(spec.get("oil_range"), (0.0, 100.0)),
            voltage=self._parse_range(spec.get("voltage_range"), (10.0, 16.0)),
            fuel_raw=self._parse_range(spec.get("fuel_raw_range"), (0.0, 1023.0)),
            temp_raw=self._parse_range(spec.get("temp_raw_range"), (0.0, 1023.0)),
            ambient_temp_raw=self._parse_range(spec.get("ambient_temp_raw_range"), (0.0, 1023.0)),
            oil_raw=self._parse_range(spec.get("oil_raw_range"), (0.0, 1023.0)),
            voltage_raw=self._parse_range(spec.get("voltage_raw_range"), (0.0, 1023.0)),
        )
        if not self._enabled:
            return
        if serial is None:
            self._enabled = False
            return
        requested_port = str(spec.get("port", "/dev/serial0")).strip()
        for candidate in (
            requested_port,
            "/dev/serial0",
            "/dev/ttyAMA0",
            "/dev/ttyS0",
            "/dev/ttyAMA3",
            "/dev/ttyUSB0",
            "/dev/ttyACM0",
        ):
            if candidate and candidate not in self._port_candidates:
                self._port_candidates.append(candidate)

        self._try_open_port(force=True)

    def is_enabled(self) -> bool:
        return bool(self._enabled)

    def _try_open_port(self, *, force: bool) -> None:
        if self._port is not None or not self._enabled:
            return
        now = time.monotonic()
        if not force and (now - self._last_open_attempt_monotonic) < self._open_retry_interval_s:
            return
        self._last_open_attempt_monotonic = now

        for port_name in self._port_candidates:
            if not os.path.exists(port_name):
                continue
            try:
                self._port = serial.Serial(port_name, baudrate=self._baudrate, timeout=self._timeout_s)
                self._buffer.clear()
                self._last_handshake_sent_monotonic = 0.0
                return
            except Exception:
                self._port = None

    def _drop_port(self) -> None:
        if self._port is None:
            return
        try:
            self._port.close()
        except Exception:
            pass
        self._port = None

    def _parse_range(self, raw: Any, fallback: tuple[float, float]) -> tuple[float, float]:
        if not isinstance(raw, list) or len(raw) != 2:
            return fallback
        lo = float(raw[0])
        hi = float(raw[1])
        if hi <= lo:
            return fallback
        return (lo, hi)

    def _map_adc(self, raw: int, in_range: tuple[float, float], out_range: tuple[float, float]) -> float:
        in_min, in_max = in_range
        ratio = 0.0 if in_max <= in_min else (float(raw) - in_min) / (in_max - in_min)
        ratio = max(0.0, min(1.0, ratio))
        return out_range[0] + ((out_range[1] - out_range[0]) * ratio)

    def _thermistor_temp_f(self, raw_adc: int) -> float | None:
        # Match sensor_pack.ino conversion constants (100k NTC, B3950).
        if raw_adc <= 0 or raw_adc >= 1023:
            return None
        therm_series_r = 100000.0
        therm_nominal_r = 100000.0
        therm_nominal_t_c = 25.0
        therm_beta = 3950.0

        resistance = therm_series_r * ((1023.0 / float(raw_adc)) - 1.0)
        if resistance <= 0.0:
            return None
        steinhart = math.log(resistance / therm_nominal_r) / therm_beta
        steinhart += 1.0 / (therm_nominal_t_c + 273.15)
        temp_c = (1.0 / steinhart) - 273.15
        if not math.isfinite(temp_c):
            return None
        return (temp_c * 9.0 / 5.0) + 32.0

    def _parse_frame(self, payload: bytes) -> dict[str, float]:
        if len(payload) == LEGACY_PAYLOAD_LEN:
            pot = int.from_bytes(payload[0:2], "big")
            therm = int.from_bytes(payload[2:4], "big")
            pressure = int.from_bytes(payload[4:6], "big")
            voltage = int.from_bytes(payload[6:8], "big")
            speed_rpm = int.from_bytes(payload[8:12], "big")
            engine_rpm = int.from_bytes(payload[12:16], "big")
            flags = int(payload[16])
            faults = int.from_bytes(payload[17:19], "big")
        elif len(payload) == EXTENDED_PAYLOAD_LEN:
            pot = int.from_bytes(payload[0:2], "big")
            therm = int.from_bytes(payload[2:4], "big")
            pressure = int.from_bytes(payload[4:6], "big")
            voltage = int.from_bytes(payload[6:8], "big")
            # payload[8:10] is ambient thermistor ADC in extended frame
            speed_rpm = int.from_bytes(payload[10:14], "big")
            engine_rpm = int.from_bytes(payload[14:18], "big")
            flags = int(payload[18])
            faults = int.from_bytes(payload[19:21], "big")
        else:
            return {}

        wheel_rps = (float(speed_rpm) / self._speed_pulses_per_rev) / 60.0
        speed_mps = wheel_rps * self._wheel_circumference_m
        speed_mph = speed_mps * 2.236936
        coolant_temp_f = self._thermistor_temp_f(therm)
        if coolant_temp_f is None:
            coolant_temp_f = self._map_adc(therm, self._ranges.temp_raw, self._ranges.temp)

        out: dict[str, float] = {
            "speed_sensor.vehicle_speed": speed_mph,
            "analog_inputs.engine_rpm": float(engine_rpm),
            "analog_inputs.fuel_level": self._map_adc(pot, self._ranges.fuel_raw, self._ranges.fuel),
            "analog_inputs.coolant_temp": coolant_temp_f,
            "analog_inputs.oil_pressure": self._map_adc(pressure, self._ranges.oil_raw, self._ranges.oil),
            "indicators.oil": self._map_adc(pressure, self._ranges.oil_raw, self._ranges.oil),
            "analog_inputs.system_voltage": self._map_adc(voltage, self._ranges.voltage_raw, self._ranges.voltage),
            "indicators.alt_ok": 1.0 if (flags & 0x01) else 0.0,
            "indicators.ign_on": 1.0 if (flags & 0x02) else 0.0,
            "arduino.fault_word": float(faults),
        }
        if len(payload) == EXTENDED_PAYLOAD_LEN:
            ambient_raw = int.from_bytes(payload[8:10], "big")
            ambient_temp_f = self._thermistor_temp_f(ambient_raw)
            if ambient_temp_f is None:
                ambient_temp_f = self._map_adc(
                    ambient_raw, self._ranges.ambient_temp_raw, self._ranges.ambient_temp
                )
            out["analog_inputs.ambient_temp"] = ambient_temp_f
        return out

    def _send_handshake(self, *, force: bool) -> None:
        if self._port is None:
            return
        now = time.monotonic()
        if not force and (now - self._last_handshake_sent_monotonic) < PI_HANDSHAKE_INTERVAL_S:
            return
        try:
            self._port.write(PI_HANDSHAKE_BYTE)
            self._last_handshake_sent_monotonic = now
        except Exception:
            # Drop dead serial handles so a later poll can re-open.
            self._drop_port()
            return

    def _extract_latest_packet(self) -> dict[str, float]:
        if self._port is None:
            return {}
        try:
            chunk = self._port.read(256)
        except Exception:
            self._drop_port()
            return {}
        if chunk:
            self._buffer.extend(chunk)
        latest: dict[str, float] = {}
        while len(self._buffer) >= (2 + LEGACY_PAYLOAD_LEN + 1):
            sync_idx = self._buffer.find(SYNC)
            if sync_idx < 0:
                self._buffer.clear()
                break
            if sync_idx > 0:
                del self._buffer[:sync_idx]
            if len(self._buffer) < (2 + LEGACY_PAYLOAD_LEN + 1):
                break
            frame_consumed = False
            for payload_len in (EXTENDED_PAYLOAD_LEN, LEGACY_PAYLOAD_LEN):
                frame_len = 2 + payload_len + 1
                if len(self._buffer) < frame_len:
                    continue
                payload = bytes(self._buffer[2 : 2 + payload_len])
                recv_crc = self._buffer[2 + payload_len]
                calc_crc = 0
                for byte in payload:
                    calc_crc ^= byte
                if recv_crc != calc_crc:
                    continue
                del self._buffer[:frame_len]
                parsed = self._parse_frame(payload)
                if parsed:
                    latest = parsed
                    self._send_handshake(force=True)
                frame_consumed = True
                break
            if not frame_consumed:
                # Bad sync candidate, drop one byte and keep scanning.
                del self._buffer[0]
        self._send_handshake(force=False)
        return latest

    def poll(self) -> dict[str, float]:
        if not self._enabled:
            return {}
        self._try_open_port(force=False)
        return self._extract_latest_packet()
