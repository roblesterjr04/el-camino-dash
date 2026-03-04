#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
import time

try:
    import serial  # type: ignore
except Exception as exc:  # pragma: no cover
    print(f"pyserial import failed: {exc}", file=sys.stderr)
    sys.exit(1)


SYNC = b"\xAA\x55"
PAYLOAD_LEN = 19
FRAME_LEN = 2 + PAYLOAD_LEN + 1

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
]


def parse_frame(payload: bytes) -> dict[str, int]:
    return {
        "pot": int.from_bytes(payload[0:2], "big"),
        "therm": int.from_bytes(payload[2:4], "big"),
        "pressure": int.from_bytes(payload[4:6], "big"),
        "voltage": int.from_bytes(payload[6:8], "big"),
        "speed_rpm": int.from_bytes(payload[8:12], "big"),
        "engine_rpm": int.from_bytes(payload[12:16], "big"),
        "flags": payload[16],
        "faults": int.from_bytes(payload[17:19], "big"),
    }


def to_system_voltage(raw_adc: int) -> float:
    vdiv_r1 = 27000.0
    vdiv_r2 = 10000.0
    vref = 5.0
    ratio = vdiv_r2 / (vdiv_r1 + vdiv_r2)
    return ((float(raw_adc) / 1023.0) * vref) / ratio


def decode_faults(fault_word: int) -> list[str]:
    active: list[str] = []
    for bit, name in FAULT_BITS:
        if fault_word & (1 << bit):
            active.append(name)
    return active


def render_dashboard(fields: dict[str, int]) -> None:
    alt_ok = bool(fields["flags"] & 0x01)
    ign_on = bool(fields["flags"] & 0x02)
    volts = to_system_voltage(fields["voltage"])
    faults = decode_faults(fields["faults"])

    lines: list[str] = []
    lines.append("sensor_pack live monitor (press Ctrl+C to stop)")
    lines.append("")
    lines.append("ADC raw:")
    lines.append(
        "  pot={pot:4d}  therm={therm:4d}  pressure={pressure:4d}  voltage={voltage:4d}".format(
            **fields
        )
    )
    lines.append("")
    lines.append("Computed:")
    lines.append(
        "  speed_rpm={speed_rpm:5d}  engine_rpm={engine_rpm:5d}  system_voltage={volts:5.2f}V".format(
            volts=volts, **fields
        )
    )
    lines.append(
        "  flags=0x{flags:02X}  alt_ok={alt_ok}  ign_on={ign_on}".format(
            alt_ok=int(alt_ok), ign_on=int(ign_on), **fields
        )
    )
    lines.append("")
    lines.append("Faults:")
    if faults:
        lines.append("  fault_word=0x{faults:04X}".format(**fields))
        for name in faults:
            lines.append(f"  - {name}")
    else:
        lines.append("  none (fault_word=0x0000)")

    # Redraw in-place without scrolling.
    sys.stdout.write("\x1b[H\x1b[2J")
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode sensor_pack.ino binary serial stream.")
    parser.add_argument("--port", required=True, help="Serial port, e.g. /dev/cu.usbmodem101")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--timeout", type=float, default=0.1, help="Read timeout seconds")
    parser.add_argument(
        "--refresh-hz",
        type=float,
        default=10.0,
        help="Dashboard refresh rate in Hz (default: 10)",
    )
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, baudrate=args.baud, timeout=args.timeout)
    except Exception as exc:
        print(f"Failed to open serial port {args.port}: {exc}", file=sys.stderr)
        return 1

    print(f"Listening on {args.port} @ {args.baud}...")
    buf = bytearray()
    last_render = 0.0
    refresh_interval = 1.0 / max(1.0, args.refresh_hz)
    latest_fields: dict[str, int] | None = None

    try:
        while True:
            chunk = ser.read(256)
            if chunk:
                buf.extend(chunk)
            while len(buf) >= FRAME_LEN:
                sync_idx = buf.find(SYNC)
                if sync_idx < 0:
                    buf.clear()
                    break
                if sync_idx > 0:
                    del buf[:sync_idx]
                if len(buf) < FRAME_LEN:
                    break

                payload = bytes(buf[2 : 2 + PAYLOAD_LEN])
                recv_crc = buf[2 + PAYLOAD_LEN]
                del buf[:FRAME_LEN]

                calc_crc = 0
                for b in payload:
                    calc_crc ^= b
                if recv_crc != calc_crc:
                    continue

                latest_fields = parse_frame(payload)

            now = time.time()
            if latest_fields is not None and (now - last_render) >= refresh_interval:
                render_dashboard(latest_fields)
                last_render = now
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())
