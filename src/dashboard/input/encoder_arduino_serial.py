from __future__ import annotations

import os
import time
from typing import Any

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    serial = None


class EncoderArduinoSerialInput:
    """Consumes rotary/button events from a dedicated Arduino serial link."""

    def __init__(self, spec: dict[str, Any]) -> None:
        self._spec = spec
        self._enabled = bool(spec.get("enabled", False))
        self._port = None
        self._open_port_name = ""
        self._buffer = bytearray()
        self._baudrate = int(spec.get("baudrate", 115200))
        self._timeout_s = float(spec.get("timeout_s", 0.0))
        self._last_open_attempt_monotonic = 0.0
        self._open_retry_interval_s = max(0.5, float(spec.get("open_retry_interval_s", 1.0)))
        self._last_open_warning_monotonic = 0.0
        self._last_read_error_monotonic = 0.0
        self._last_frame_log_monotonic = 0.0
        self._last_poll_log_monotonic = 0.0
        self._port_candidates: list[str] = []
        self._menu_presses = 0.0
        self._aux_presses = 0.0
        self._last_frame = "-"
        self._rx_bytes_total = 0
        self._rx_lines_total = 0
        self._rx_parsed_frames_total = 0
        self._poll_count = 0

        if not self._enabled:
            return
        if serial is None:
            self._enabled = False
            return

        requested_port = str(spec.get("port", "/dev/ttyAMA4")).strip()
        for candidate in (
            requested_port,
            "/dev/ttyAMA4",
            "/dev/ttyAMA3",
            "/dev/ttyUSB1",
            "/dev/ttyACM1",
        ):
            if candidate and candidate not in self._port_candidates:
                self._port_candidates.append(candidate)

        self._try_open_port(force=True)

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
                self._open_port_name = port_name
                self._buffer.clear()
                print(f"[encoder-serial] opened {port_name} @ {self._baudrate}", flush=True)
                return
            except Exception:
                self._port = None
        if (now - self._last_open_warning_monotonic) >= 5.0:
            joined = ", ".join(self._port_candidates)
            print(f"[encoder-serial] waiting for port (tried: {joined})", flush=True)
            self._last_open_warning_monotonic = now

    def _drop_port(self) -> None:
        if self._port is None:
            return
        try:
            self._port.close()
        except Exception:
            pass
        self._port = None
        self._open_port_name = ""

    def _parse_int(self, value: str) -> int | None:
        text = value.strip()
        if not text:
            return None
        try:
            return int(text, 10)
        except ValueError:
            return None

    def _parse_line(self, line: str) -> dict[str, int]:
        # Supported event frame examples:
        # MD=1,AD=0,MP=12,AP=3
        # MD:-1 AD:0 MP:12 AP:3
        parsed: dict[str, int] = {}
        for chunk in line.replace(" ", ",").split(","):
            token = chunk.strip()
            if not token:
                continue
            sep = "=" if "=" in token else ":"
            if sep not in token:
                continue
            key, raw = token.split(sep, 1)
            key = key.strip().upper()
            value = self._parse_int(raw)
            if value is None:
                continue
            parsed[key] = value
        return parsed

    def poll(self) -> dict[str, float]:
        if not self._enabled:
            return {}

        self._poll_count += 1
        self._try_open_port(force=False)
        menu_delta = 0.0
        aux_delta = 0.0

        if self._port is not None:
            try:
                chunk = self._port.read(256)
            except Exception as exc:
                now = time.monotonic()
                if (now - self._last_read_error_monotonic) >= 5.0:
                    print(f"[encoder-serial] read error on {self._open_port_name or '?'}: {exc}", flush=True)
                    self._last_read_error_monotonic = now
                self._drop_port()
                chunk = b""
            if chunk:
                # Arduino println uses CRLF; normalize to LF so framing survives
                # if one newline byte is intermittently dropped.
                chunk = chunk.replace(b"\r", b"\n")
                self._rx_bytes_total += len(chunk)
                self._buffer.extend(chunk)

        now = time.monotonic()
        if (now - self._last_poll_log_monotonic) >= 2.0:
            print(
                "[encoder-serial] poll "
                f"count={self._poll_count} port={self._open_port_name or '-'} "
                f"bytes={self._rx_bytes_total} lines={self._rx_lines_total} frames={self._rx_parsed_frames_total}",
                flush=True,
            )
            self._last_poll_log_monotonic = now

        while True:
            newline_idx = self._buffer.find(b"\n")
            if newline_idx < 0:
                break
            raw_line = bytes(self._buffer[:newline_idx])
            del self._buffer[: newline_idx + 1]
            line = raw_line.decode("utf-8", errors="ignore").strip()
            if not line:
                continue
            self._rx_lines_total += 1
            self._last_frame = line
            now = time.monotonic()
            if (now - self._last_frame_log_monotonic) >= 1.0:
                print(f"[encoder-serial] frame: {line}", flush=True)
                self._last_frame_log_monotonic = now
            fields = self._parse_line(line)
            if not fields:
                continue
            self._rx_parsed_frames_total += 1
            menu_delta += float(fields.get("MD", 0))
            aux_delta += float(fields.get("AD", 0))
            if "MP" in fields:
                next_menu = float(max(0, fields["MP"]))
                self._menu_presses = next_menu
            if "AP" in fields:
                next_aux = float(max(0, fields["AP"]))
                self._aux_presses = next_aux

        return {
            "inputs.rotary.menu.delta": float(menu_delta),
            "inputs.rotary.menu.presses": float(self._menu_presses),
            "inputs.rotary.aux.delta": float(aux_delta),
            "inputs.rotary.aux.presses": float(self._aux_presses),
        }

    @property
    def last_frame(self) -> str:
        if self._last_frame != "-":
            return self._last_frame
        if self._rx_bytes_total > 0:
            return (
                f"rx:{self._rx_bytes_total} lines:{self._rx_lines_total} "
                f"frames:{self._rx_parsed_frames_total}"
            )
        return "-"
