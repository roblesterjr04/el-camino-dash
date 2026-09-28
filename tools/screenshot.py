#!/usr/bin/env python3
"""Grab what an ESP32-P4 display is showing, over its USB serial port.

Sends `shot` to the firmware console, follows its switch to a fast baud rate, reads the raw RGB565
framebuffer and saves a PNG (needs Pillow; without it a .ppm is written).

    python3 tools/screenshot.py                       # auto-detect port, writes screenshots/<time>.png
    python3 tools/screenshot.py --port /dev/cu.usbmodem5ABA0545011 -o center.png
"""
from __future__ import annotations

import argparse
import glob
import sys
import time
import zlib
from pathlib import Path

try:
    import serial  # type: ignore
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    if not ports:
        sys.exit("No USB serial port found; pass --port")
    return ports[0]


def read_line(ser: serial.Serial, deadline: float) -> str:
    buf = bytearray()
    while time.time() < deadline:
        c = ser.read(1)
        if not c:
            continue
        if c == b"\n":
            return buf.decode("ascii", "replace").strip()
        buf += c
    raise TimeoutError("timed out waiting for the device")


def grab(port: str, baud: int, timeout: float, cmd: bytes = b"shot") -> tuple[int, int, bytes]:
    # Open with the OS defaults and never touch DTR/RTS: on the Waveshare P4 boards the CH343's
    # auto-reset circuit reboots the ESP32 when one line changes without the other.
    ser = serial.Serial(port, baud, timeout=0.2)
    with ser:
        ser.reset_input_buffer()
        ser.write(b"\n" + cmd + b"\n")
        deadline = time.time() + timeout
        resend_at = time.time() + 3
        while True:
            try:
                line = read_line(ser, min(deadline, resend_at))
            except TimeoutError:
                if time.time() >= deadline:
                    raise
                ser.write(b"\n" + cmd + b"\n")  # the board may have been busy or booting
                resend_at = time.time() + 3
                continue
            if line.startswith("SHOT BAUD "):
                ser.baudrate = int(line.split()[2])
                ser.reset_input_buffer()
                continue
            if line.startswith("SHOT ERR"):
                sys.exit(line)
            if line.startswith("SHOT ") and "RGB565LE" in line:
                _, w, h, _, n = line.split()
                w, h, n = int(w), int(h), int(n)
                break
        data = bytearray()
        while len(data) < n and time.time() < deadline:
            data += ser.read(n - len(data))
        if len(data) < n:
            raise TimeoutError(f"got {len(data)} of {n} bytes")
        end = read_line(ser, deadline)
        if not end:
            end = read_line(ser, deadline)
        ser.baudrate = baud
    crc_dev = end.split()[-1] if end.startswith("SHOT END") else None
    crc_host = f"{zlib.crc32(bytes(data)) & 0xFFFFFFFF:08x}"
    if crc_dev != crc_host:
        print(f"warning: checksum mismatch (device {crc_dev}, host {crc_host}); the image may be corrupted", file=sys.stderr)
    return w, h, bytes(data)


def to_rgb888(w: int, h: int, data: bytes) -> bytes:
    out = bytearray(w * h * 3)
    for i in range(w * h):
        v = data[2 * i] | (data[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        out[3 * i] = (r << 3) | (r >> 2)
        out[3 * i + 1] = (g << 2) | (g >> 4)
        out[3 * i + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("-o", "--out")
    ap.add_argument("--legacy", action="store_true", help="dump the legacy drawing layer instead of the panel")
    args = ap.parse_args()

    port = args.port or find_port()
    t0 = time.time()
    w, h, data = grab(port, args.baud, args.timeout, b"shot legacy" if args.legacy else b"shot")
    rgb = to_rgb888(w, h, data)
    out = Path(args.out) if args.out else Path("screenshots") / time.strftime("%Y%m%d-%H%M%S.png")
    out.parent.mkdir(parents=True, exist_ok=True)
    try:
        from PIL import Image  # type: ignore

        Image.frombytes("RGB", (w, h), rgb).save(out)
    except ImportError:
        out = out.with_suffix(".ppm")
        out.write_bytes(f"P6 {w} {h} 255\n".encode() + rgb)
    print(f"{out} ({w}x{h}, {time.time() - t0:.1f}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
