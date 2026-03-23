"""Vehicle telemetry from Arduino Mega over serial JSON lines."""

from __future__ import annotations

import json
import logging
import threading
import time
from typing import Any

import serial

from config import (
    TELEMETRY_BAUD_RATE,
    TELEMETRY_SERIAL_PORT,
    TELEMETRY_TIMEOUT_S,
    TELEMETRY_UPDATE_INTERVAL_S,
)

logger = logging.getLogger(__name__)

_lock = threading.Lock()
_latest: dict[str, Any] = {
    "rpm": None,
    "speed_mph": None,
    "coolant_f": None,
    "oil_psi": None,
    "voltage": None,
    "faults": None,
}
_warned_serial = False
_stop = threading.Event()
_thread: threading.Thread | None = None


def _reader_loop() -> None:
    global _warned_serial
    ser = None
    while not _stop.is_set():
        if ser is None:
            try:
                ser = serial.Serial(
                    TELEMETRY_SERIAL_PORT,
                    TELEMETRY_BAUD_RATE,
                    timeout=TELEMETRY_TIMEOUT_S,
                )
            except OSError as exc:
                if not _warned_serial:
                    logger.warning(
                        "Telemetry serial unavailable (%s): %s",
                        TELEMETRY_SERIAL_PORT,
                        exc,
                    )
                    _warned_serial = True
                time.sleep(TELEMETRY_UPDATE_INTERVAL_S)
                continue
            _warned_serial = False
        try:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").strip()
            if not text:
                continue
            data = json.loads(text)
            with _lock:
                for k in list(_latest.keys()):
                    if k in data:
                        _latest[k] = data[k]
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            logger.debug("Bad telemetry line: %s", exc)
        except OSError:
            try:
                ser.close()
            except OSError:
                pass
            ser = None
        except Exception as exc:
            logger.warning("Telemetry read error: %s", exc)
            time.sleep(0.2)


def start() -> None:
    """Start the background telemetry reader thread."""
    global _thread
    if _thread is not None and _thread.is_alive():
        return
    _stop.clear()
    _thread = threading.Thread(target=_reader_loop, name="telemetry", daemon=True)
    _thread.start()


def get_telemetry() -> dict:
    with _lock:
        return dict(_latest)


def format_telemetry_for_prompt(data: dict) -> str:
    if not data or all(data.get(k) is None for k in data):
        return "Vehicle data unavailable"
    rpm = data.get("rpm")
    spd = data.get("speed_mph")
    cool = data.get("coolant_f")
    oil = data.get("oil_psi")
    volt = data.get("voltage")
    faults = data.get("faults")
    if faults is None:
        fault_s = "unknown"
    elif isinstance(faults, list) and len(faults) == 0:
        fault_s = "none"
    elif isinstance(faults, list):
        fault_s = ", ".join(str(f) for f in faults)
    else:
        fault_s = str(faults)
    return (
        f"RPM: {rpm}  |  Speed: {spd} mph  |  Coolant: {cool}°F  |  "
        f"Oil: {oil} PSI  |  Battery: {volt}V  |  Faults: {fault_s}"
    )
