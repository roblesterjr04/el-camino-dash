#!/usr/bin/env python3
"""Live decoder for the sensor_pack UART stream (bench tool).

Wire format: docs/protocols/sensor_uart.md — AA 55 + 33-byte big-endian payload + XOR = 36 bytes.
Text lines (boot banner, FRAM status) between frames are printed below the dashboard.
"""
from __future__ import annotations

import argparse
import glob
import math
import sys
import time

try:
    import serial  # type: ignore
except Exception as exc:  # pragma: no cover
    print(f"pyserial import failed: {exc}", file=sys.stderr)
    sys.exit(1)


SYNC = b"\xAA\x55"
PAYLOAD_LEN = 33
FRAME_LEN = 2 + PAYLOAD_LEN + 1
ODOMETER_UNKNOWN = 0xFFFFFFFF

FAULT_BITS: list[tuple[int, str]] = [
    (0, "POT_OPEN"),
    (1, "POT_SHORT"),
    (2, "TEMP_OPEN"),
    (3, "TEMP_SHORT"),
    (4, "PRESS_OPEN"),
    (5, "PRESS_SHORT"),
    (6, "VOLT_OPEN"),
    (7, "VOLT_HIGH"),
    (8, "HALL_MISSING"),
    (9, "COIL_MISSING"),
    (10, "ALT_STUCK"),
    (11, "POT_POWER"),
    (12, "TEMP_POWER"),
    (13, "PRESS_POWER"),
    (14, "VOLT_POWER"),
    (15, "ODO_STORE"),
]

MAC_PORT_GLOBS: tuple[str, ...] = (
    "/dev/cu.usbmodem*",
    "/dev/cu.usbserial*",
    "/dev/cu.SLAB_USBtoUART*",
    "/dev/cu.wchusbserial*",
)


def parse_frame(payload: bytes) -> dict[str, int | None]:
    if len(payload) != PAYLOAD_LEN:
        raise ValueError(f"Unexpected payload length: {len(payload)}")
    be = lambda lo, hi: int.from_bytes(payload[lo:hi], "big")  # noqa: E731
    odo = be(21, 25)
    return {
        "pot": be(0, 2),
        "therm": be(2, 4),
        "pressure": be(4, 6),
        "voltage": be(6, 8),
        "ambient_therm": be(8, 10),
        "speed_rpm": be(10, 14),
        "engine_rpm": be(14, 18),
        "flags": payload[18],
        "faults": be(19, 21),
        "odo_tenths": None if odo == ODOMETER_UNKNOWN else odo,
        "mpg_inst_x10": be(25, 27),
        "mpg_avg_x10": be(27, 29),
        "dte_tenths": be(29, 33),
    }


def charge_band(raw_adc: int) -> str:
    # sensor_pack synthesizes this slot from IGN + ALT "L" (no voltage divider): 400 / 530 / 720.
    return {400: "key off", 530: "ign on, not charging", 720: "charging"}.get(raw_adc, f"raw {raw_adc}")


def thermistor_temp_c(raw_adc: int) -> float:
    # Match sensor_pack.ino conversion constants (100k NTC, B3950).
    therm_series_r = 100000.0
    therm_nominal_r = 100000.0
    therm_nominal_t_c = 25.0
    therm_beta = 3950.0

    if raw_adc <= 0 or raw_adc >= 1023:
        return float("nan")
    resistance = therm_series_r * ((1023.0 / float(raw_adc)) - 1.0)
    if resistance <= 0.0:
        return float("nan")

    steinhart = math.log(resistance / therm_nominal_r) / therm_beta
    steinhart += 1.0 / (therm_nominal_t_c + 273.15)
    return (1.0 / steinhart) - 273.15


def temp_c_to_f(temp_c: float) -> float:
    return (temp_c * 9.0 / 5.0) + 32.0


def fmt_temp(raw_adc: int | None) -> str:
    if raw_adc is None:
        return "n/a"
    temp_c = thermistor_temp_c(raw_adc)
    if math.isnan(temp_c):
        return "invalid"
    temp_f = temp_c_to_f(temp_c)
    return f"{temp_c:5.1f}C / {temp_f:5.1f}F"


def decode_faults(fault_word: int) -> list[str]:
    active: list[str] = []
    for bit, name in FAULT_BITS:
        if fault_word & (1 << bit):
            active.append(name)
    return active


def list_serial_ports() -> list[str]:
    ports: list[str] = []
    try:
        from serial.tools import list_ports  # type: ignore

        ports = sorted(p.device for p in list_ports.comports() if p.device)
    except Exception:
        # Fall back to a direct /dev glob on hosts where list_ports is unavailable.
        if sys.platform == "darwin":
            for pattern in MAC_PORT_GLOBS:
                ports.extend(glob.glob(pattern))
            ports = sorted(set(ports))
    return ports


def resolve_port(port_arg: str | None) -> str | None:
    if port_arg:
        return port_arg

    ports = list_serial_ports()
    if not ports:
        return None

    if len(ports) == 1:
        return ports[0]

    # Prefer common Arduino-style ports first when multiple are available.
    preferred = [p for p in ports if "usbmodem" in p or "usbserial" in p]
    if preferred:
        return preferred[0]
    return ports[0]


def render_dashboard(fields: dict[str, int | None], text_lines: list[str]) -> None:
    flags = int(fields["flags"] or 0)
    faults_word = int(fields["faults"] or 0)
    voltage_raw = int(fields["voltage"] or 0)
    therm_raw = int(fields["therm"] or 0)
    ambient_raw = fields["ambient_therm"]
    speed_rpm = int(fields["speed_rpm"] or 0)
    engine_rpm = int(fields["engine_rpm"] or 0)
    alt_ok = bool(flags & 0x01)
    ign_on = bool(flags & 0x02)
    faults = decode_faults(faults_word)

    lines: list[str] = []
    lines.append("sensor_pack live monitor (press Ctrl+C to stop)")
    lines.append("")
    lines.append("ADC raw:")
    lines.append(
        "  pot={pot:4d}  therm={therm:4d}  ambient={ambient:>4}  pressure={pressure:4d}  voltage={voltage:4d}".format(
            pot=int(fields["pot"] or 0),
            therm=therm_raw,
            ambient=("n/a" if ambient_raw is None else f"{int(ambient_raw):4d}"),
            pressure=int(fields["pressure"] or 0),
            voltage=voltage_raw,
        )
    )
    lines.append("")
    lines.append("Computed:")
    lines.append(
        "  speed_rpm={speed_rpm:5d}  engine_rpm={engine_rpm:5d}  charge={charge}".format(
            speed_rpm=speed_rpm,
            engine_rpm=engine_rpm,
            charge=charge_band(voltage_raw),
        )
    )
    lines.append(f"  engine_temp={fmt_temp(therm_raw)}  ambient_temp={fmt_temp(ambient_raw)}")
    odo = fields["odo_tenths"]
    lines.append(
        "  odometer={odo}  mpg_inst={mi:.1f}  mpg_avg={ma:.1f}  dte={dte:.1f} mi".format(
            odo="UNKNOWN (FRAM)" if odo is None else f"{int(odo) / 10:.1f} mi",
            mi=int(fields["mpg_inst_x10"] or 0) / 10,
            ma=int(fields["mpg_avg_x10"] or 0) / 10,
            dte=int(fields["dte_tenths"] or 0) / 10,
        )
    )
    lines.append(
        "  flags=0x{flags:02X}  alt_ok={alt_ok}  ign_on={ign_on}".format(
            flags=flags,
            alt_ok=int(alt_ok),
            ign_on=int(ign_on),
        )
    )
    lines.append("")
    lines.append("Faults:")
    if faults:
        lines.append(f"  fault_word=0x{faults_word:04X}")
        for name in faults:
            lines.append(f"  - {name}")
    else:
        lines.append("  none (fault_word=0x0000)")
    if text_lines:
        lines.append("")
        lines.append("Recent text lines:")
        lines.extend(f"  {t}" for t in text_lines[-8:])

    # Redraw in-place without scrolling.
    sys.stdout.write("\x1b[H\x1b[2J")
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()


def drain_buffer(buf: bytearray) -> list[tuple[str, object]]:
    """Consume complete frames and text lines from the front of `buf` (mirrors sensor_packet.cpp)."""
    out: list[tuple[str, object]] = []
    while buf:
        if buf[:2] == SYNC or buf == SYNC[:1]:
            if len(buf) < FRAME_LEN:
                break
            payload = bytes(buf[2 : 2 + PAYLOAD_LEN])
            crc = 0
            for b in payload:
                crc ^= b
            if crc == buf[2 + PAYLOAD_LEN]:
                out.append(("frame", parse_frame(payload)))
                del buf[:FRAME_LEN]
            else:
                del buf[0]
            continue
        nl = buf.find(b"\n")
        sync = buf.find(SYNC, 1)
        if sync >= 0 and (nl < 0 or sync < nl):
            del buf[:sync]  # unterminated junk before a frame
            continue
        if nl < 0:
            if len(buf) > 256:
                del buf[0]
                continue
            break
        line = bytes(buf[:nl]).rstrip(b"\r")
        del buf[: nl + 1]
        if line and all(0x20 <= c <= 0x7E for c in line):
            out.append(("text", line.decode("ascii")))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode sensor_pack.ino binary serial stream.")
    parser.add_argument(
        "--port",
        help="Serial port, e.g. /dev/cu.usbmodem101. If omitted, auto-detect is attempted.",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--timeout", type=float, default=0.1, help="Read timeout seconds")
    parser.add_argument(
        "--list-ports",
        action="store_true",
        help="List detected serial ports and exit",
    )
    parser.add_argument(
        "--refresh-hz",
        type=float,
        default=10.0,
        help="Dashboard refresh rate in Hz (default: 10)",
    )
    args = parser.parse_args()

    if args.list_ports:
        ports = list_serial_ports()
        if ports:
            print("Detected serial ports:")
            for p in ports:
                print(f"  {p}")
            return 0
        print("No serial ports detected.")
        return 1

    port = resolve_port(args.port)
    if not port:
        print(
            "No serial port specified and auto-detect failed.\n"
            "Use --list-ports to discover available devices, then pass --port.",
            file=sys.stderr,
        )
        return 1

    try:
        ser = serial.Serial(port, baudrate=args.baud, timeout=args.timeout)
    except Exception as exc:
        print(f"Failed to open serial port {port}: {exc}", file=sys.stderr)
        return 1

    print(f"Listening on {port} @ {args.baud}...")
    buf = bytearray()
    last_render = 0.0
    refresh_interval = 1.0 / max(1.0, args.refresh_hz)
    latest_fields: dict[str, int | None] | None = None
    text_lines: list[str] = []

    try:
        while True:
            chunk = ser.read(256)
            if chunk:
                buf.extend(chunk)
            for kind, value in drain_buffer(buf):
                if kind == "frame":
                    latest_fields = value
                else:
                    text_lines.append(value)
                    del text_lines[:-50]
                    if latest_fields is None:
                        print(value)

            now = time.time()
            if latest_fields is not None and (now - last_render) >= refresh_interval:
                render_dashboard(latest_fields, text_lines)
                last_render = now
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())
