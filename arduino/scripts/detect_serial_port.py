#!/usr/bin/env python3
"""Pick a serial port for arduino-cli upload. Skips Bluetooth; prefers USB CDC/serial."""

from __future__ import annotations

import json
import re
import subprocess
import sys

SKIP = re.compile(r"Bluetooth|debug-console|wlan", re.I)
USBISH = re.compile(
    r"usbmodem|usbserial|tty\.usb|ttyACM|ttyUSB|wchusb|SLAB|serial0$",
    re.I,
)


def main() -> int:
    prefer = sys.argv[1] if len(sys.argv) > 1 else "any"
    if prefer not in ("any", "avr", "esp32", "pico"):
        print(f"Usage: {sys.argv[0]} [any|avr|esp32|pico]", file=sys.stderr)
        return 2

    r = subprocess.run(
        ["arduino-cli", "board", "list", "--format", "json"],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(r.stderr or "arduino-cli board list failed", file=sys.stderr)
        return 1

    try:
        data = json.loads(r.stdout or "{}")
    except json.JSONDecodeError as e:
        print(f"Invalid JSON from arduino-cli: {e}", file=sys.stderr)
        return 1

    rows: list[tuple[str, str]] = []
    for entry in data.get("detected_ports") or []:
        port = entry.get("port") or {}
        addr = (port.get("address") or "").strip()
        if not addr.startswith("/dev/"):
            continue
        if SKIP.search(addr):
            continue
        boards = entry.get("matching_boards") or []
        fqbn = ""
        if boards:
            fqbn = (boards[0].get("fqbn") or "").strip()
        rows.append((addr, fqbn))

    if not rows:
        print(
            "No usable serial ports found. Connect a board and run: arduino-cli board list",
            file=sys.stderr,
        )
        return 1

    def score(item: tuple[str, str]) -> tuple[int, str]:
        addr, fqbn = item
        s = 0
        if USBISH.search(addr):
            s += 20
        fl = fqbn.lower()
        if prefer == "esp32" and "esp32" in fl:
            s += 10
        if prefer == "avr" and "avr" in fl:
            s += 10
        if prefer == "pico" and ("rp2040" in fl or "mbed_rp2040" in fl or "rpi pico" in fl):
            s += 10
        return (-s, addr)

    rows.sort(key=score)
    best = score(rows[0])
    tied = [p for p in rows if score(p) == best]

    if len(tied) > 1:
        print("Multiple matching serial ports; pass one explicitly:", file=sys.stderr)
        for addr, fqbn in tied:
            extra = f" ({fqbn})" if fqbn else ""
            print(f"  {addr}{extra}", file=sys.stderr)
        return 1

    print(rows[0][0])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
