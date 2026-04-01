from __future__ import annotations

import os
import threading
import time
from typing import Any

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover
    serial = None

# Pi-side tokens from encoder MCU IR map (see encoder_controller.ino applyNecCommand).
_IR_TOKEN_KEYS: dict[str, str] = {
    "volume_up": "inputs.ir.command.volume_up_count",
    "volume_down": "inputs.ir.command.volume_down_count",
    "theme_next": "inputs.ir.command.theme_next_count",
    "map_toggle": "inputs.ir.command.map_toggle_count",
    "center_toggle": "inputs.ir.command.center_toggle_count",
    "tune_up": "inputs.ir.command.tune_up_count",
    "tune_down": "inputs.ir.command.tune_down_count",
    "dash_exit": "inputs.ir.command.dash_exit_count",
    "menu_open": "inputs.ir.command.menu_open_count",
    "diagnostics_open": "inputs.ir.command.diagnostics_open_count",
}


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
        self._write_lock = threading.Lock()
        self._ir_counts: dict[str, float] = {key: 0.0 for key in _IR_TOKEN_KEYS.values()}
        self._last_ir_code = "-"

        if not self._enabled:
            return
        if serial is None:
            self._enabled = False
            return

        env_port = os.environ.get("EL_CAMINO_ENCODER_SERIAL_PORT", "").strip()
        requested_port = env_port or str(spec.get("port", "/dev/serial0")).strip()
        # Vehicle UART4 encoder link is often /dev/ttyAMA4. If config still says /dev/serial0
        # (missing on many Pi images) or opening the wrong ttyAMA*, try ttyAMA4 before ttyAMA0.
        for candidate in (
            requested_port,
            "/dev/ttyAMA4",
            "/dev/serial0",
            "/dev/ttyAMA0",
            "/dev/ttyS0",
            "/dev/ttyUSB0",
            "/dev/ttyACM0",
        ):
            if candidate and candidate not in self._port_candidates:
                self._port_candidates.append(candidate)

        self._try_open_port(force=True)

    def is_enabled(self) -> bool:
        return bool(self._enabled)

    def write_raw(self, data: bytes) -> None:
        """Write bytes on the encoder UART (shared with Nano OLED downlink). Thread-safe."""
        if not self._enabled or not data:
            return
        self._try_open_port(force=False)
        if self._port is None:
            return
        with self._write_lock:
            try:
                self._port.write(data)
                self._port.flush()
            except Exception:
                self._drop_port()

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

    @staticmethod
    def _rotary_step_delta(raw: str) -> float | None:
        """MCU sends UP/DOWN per detent; legacy frames use signed integers."""
        r = raw.strip().upper()
        if r == "UP":
            return 1.0
        if r == "DOWN":
            return -1.0
        return None

    def _parse_line(self, line: str) -> tuple[float, float, dict[str, int], list[str]]:
        # MD=UP,AD=DOWN,… | MD=UP,MD=UP,… | legacy MD=-2,AD=0 | MP=12,AP=3[,IR=volume_up]
        menu_delta = 0.0
        aux_delta = 0.0
        parsed: dict[str, int] = {}
        ir_tokens: list[str] = []
        for chunk in line.replace(" ", ",").split(","):
            token = chunk.strip()
            if not token:
                continue
            sep = "=" if "=" in token else ":"
            if sep not in token:
                continue
            key, raw = token.split(sep, 1)
            key = key.strip().upper()
            raw_stripped = raw.strip()
            if key == "IR":
                cleaned = "".join(c for c in raw_stripped.lower() if c.isalnum() or c == "_")
                if cleaned and cleaned in _IR_TOKEN_KEYS:
                    ir_tokens.append(cleaned)
                continue
            if key == "MD":
                step = self._rotary_step_delta(raw_stripped)
                if step is not None:
                    menu_delta += step
                else:
                    v = self._parse_int(raw_stripped)
                    if v is not None:
                        menu_delta += float(v)
                continue
            if key == "AD":
                step = self._rotary_step_delta(raw_stripped)
                if step is not None:
                    aux_delta += step
                else:
                    v = self._parse_int(raw_stripped)
                    if v is not None:
                        aux_delta += float(v)
                continue
            value = self._parse_int(raw_stripped)
            if value is None:
                continue
            parsed[key] = value
        return menu_delta, aux_delta, parsed, ir_tokens

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
            md_line, ad_line, fields, ir_tokens = self._parse_line(line)
            if not fields and not ir_tokens and md_line == 0.0 and ad_line == 0.0:
                continue
            self._rx_parsed_frames_total += 1
            menu_delta += md_line
            aux_delta += ad_line
            if "MP" in fields:
                next_menu = float(max(0, fields["MP"]))
                self._menu_presses = next_menu
            if "AP" in fields:
                next_aux = float(max(0, fields["AP"]))
                self._aux_presses = next_aux
            for ir_tok in ir_tokens:
                vkey = _IR_TOKEN_KEYS[ir_tok]
                self._ir_counts[vkey] = float(self._ir_counts.get(vkey, 0.0)) + 1.0
                self._last_ir_code = ir_tok

        out: dict[str, float] = {
            "inputs.rotary.menu.delta": float(menu_delta),
            "inputs.rotary.menu.presses": float(self._menu_presses),
            "inputs.rotary.aux.delta": float(aux_delta),
            "inputs.rotary.aux.presses": float(self._aux_presses),
        }
        out.update(self._ir_counts)
        return out

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

    def last_ir_code(self) -> str:
        return str(self._last_ir_code or "-")
