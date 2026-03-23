from __future__ import annotations

import time
from collections.abc import Callable
from typing import Any

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    serial = None


class NanoBridge:
    """Pi -> Nano OLED downlink. Uses the encoder UART when that input is enabled (same tty)."""

    def __init__(self, spec: dict[str, Any]) -> None:
        self._enabled = bool(spec.get("enabled", False))
        self._port_name = str(spec.get("port", "/dev/serial0"))
        self._baudrate = int(spec.get("baudrate", 115200))
        self._write_interval_s = max(0.05, float(spec.get("write_interval_s", 0.2)))
        self._port: Any | None = None
        self._serial_write: Callable[[bytes], None] | None = None
        self._last_write_s = 0.0
        self._status = "disabled"

    def bind_serial_writer(self, writer: Callable[[bytes], None] | None) -> None:
        """Share the encoder serial port (recommended on Pi: one open FD per tty)."""
        self._serial_write = writer

    def start(self) -> None:
        if not self._enabled:
            self._status = "disabled"
            return
        if self._serial_write is not None:
            self._status = "shared-serial"
            return
        if serial is None:
            self._status = "pyserial-missing"
            return
        try:
            self._port = serial.Serial(self._port_name, baudrate=self._baudrate, timeout=0.0)
            self._status = "online"
        except Exception:
            self._port = None
            self._status = "offline"

    def stop(self) -> None:
        if self._port is not None:
            try:
                self._port.close()
            except Exception:
                pass
        self._port = None

    @staticmethod
    def _oled_token(s: str, max_len: int = 40) -> str:
        t = (s or "").replace("|", " ").replace("\n", " ").replace("\r", " ")
        return t[:max_len]

    def send_oled_frame(
        self,
        *,
        mode: str,
        title: str,
        line1: str,
        line2: str,
        weather_label: str = "OUTSIDE",
        weather_temp: str = "--",
        song_title: str = "",
        song_artist: str = "",
        progress_pct: int = 0,
        pos_elapsed: str = "0:00",
        pos_total: str = "0:00",
        clock_time: str = "12:00",
        clock_ampm: str = "AM",
    ) -> None:
        if not self._enabled:
            return
        now = time.monotonic()
        if (now - self._last_write_s) < self._write_interval_s:
            return
        pr = max(0, min(100, int(progress_pct)))
        payload = (
            f"MODE={self._oled_token(mode, 8)}"
            f"|TITLE={self._oled_token(title)}"
            f"|L1={self._oled_token(line1)}"
            f"|L2={self._oled_token(line2)}"
            f"|WL={self._oled_token(weather_label, 12)}"
            f"|WT={self._oled_token(weather_temp, 8)}"
            f"|ST={self._oled_token(song_title, 31)}"
            f"|SA={self._oled_token(song_artist, 31)}"
            f"|PR={pr}"
            f"|PE={self._oled_token(pos_elapsed, 8)}"
            f"|PT={self._oled_token(pos_total, 8)}"
            f"|TM={self._oled_token(clock_time, 8)}"
            f"|AP={self._oled_token(clock_ampm, 4)}"
            "\n"
        ).encode("utf-8", errors="ignore")
        if self._serial_write is not None:
            try:
                self._serial_write(payload)
                self._last_write_s = now
                self._status = "shared-serial"
            except Exception:
                self._status = "shared-serial-error"
            return
        if self._port is None:
            return
        try:
            self._port.write(payload)
            self._last_write_s = now
            self._status = "online"
        except Exception:
            self._status = "offline"

    def status(self) -> str:
        return self._status
