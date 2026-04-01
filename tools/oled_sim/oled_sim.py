#!/usr/bin/env python3
"""
SSD1322 256×64 OLED simulator: receives the same Pi → Nano UART lines as the real dash.

Matches `encoder_controller.ino` parsing + layout (see `docs/protocols/nano_uart.md`).

Examples:
  python3 tools/oled_sim/oled_sim.py --stdin
  python3 tools/oled_sim/oled_sim.py --port /dev/ttyUSB0
  python3 tools/oled_sim/oled_sim.py --port /dev/serial0 --scale 4
"""
from __future__ import annotations

import argparse
import queue
import sys
import threading
import time
from pathlib import Path

_SIM = Path(__file__).resolve().parent
if str(_SIM) not in sys.path:
    sys.path.insert(0, str(_SIM))

from renderer import (
    OledState,
    load_fonts,
    parse_downlink_line,
    render_frame,
    buf_to_pygame_surface,
)

FONT_DIR = Path(__file__).resolve().parent / "fonts"


def read_lines_thread_serial(ser: object, q: queue.Queue[str]) -> None:
    buf = bytearray()
    try:
        while True:
            chunk = ser.read(256)
            if chunk:
                for b in chunk:
                    if b == 0x0D:
                        continue
                    if b == 0x0A:
                        line = buf.decode("utf-8", errors="replace")
                        buf.clear()
                        q.put(line)
                    else:
                        if len(buf) < 255:
                            buf.append(b)
                        else:
                            buf.clear()
            else:
                time.sleep(0.001)
    finally:
        try:
            ser.close()
        except Exception:
            pass


def read_lines_thread_stdin(q: queue.Queue[str]) -> None:
    while True:
        line = sys.stdin.readline()
        if line == "":
            break
        q.put(line.rstrip("\r\n"))


def main() -> None:
    p = argparse.ArgumentParser(description="256×64 OLED UART simulator (encoder firmware protocol)")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="Serial device (e.g. /dev/serial0, /dev/ttyUSB0)")
    src.add_argument("--stdin", action="store_true", help="Read newline-delimited lines from stdin")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--scale", type=int, default=4, help="Integer upscale for the window (default 4)")
    p.add_argument("--fps", type=int, default=60)
    args = p.parse_args()

    if not FONT_DIR.joinpath("6x10.bdf").is_file():
        print("Missing BDF fonts in", FONT_DIR, file=sys.stderr)
        sys.exit(1)

    import pygame

    pygame.init()
    pygame.display.set_caption("el-camino OLED 256×64 (sim)")
    scale = max(1, int(args.scale))
    clock = pygame.time.Clock()

    fonts = load_fonts(FONT_DIR)
    state = OledState()
    line_q: queue.Queue[str] = queue.Queue()

    if args.stdin:
        t = threading.Thread(target=read_lines_thread_stdin, args=(line_q,), daemon=True)
        t.start()
    else:
        try:
            import serial
        except ImportError:
            print("pyserial is required for --port", file=sys.stderr)
            sys.exit(1)
        try:
            ser = serial.Serial(args.port, baudrate=args.baud, timeout=0.05)
        except Exception as e:
            print(f"Could not open serial port {args.port!r}: {e}", file=sys.stderr)
            sys.exit(1)
        t = threading.Thread(target=read_lines_thread_serial, args=(ser, line_q), daemon=True)
        t.start()

    screen = pygame.display.set_mode((256 * scale, 64 * scale))
    start_ms = int(time.monotonic() * 1000)

    while True:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                pygame.quit()
                return
            if event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE:
                pygame.quit()
                return

        try:
            while True:
                line = line_q.get_nowait()
                now_ms = int(time.monotonic() * 1000) - start_ms
                parse_downlink_line(line, state, now_ms)
        except queue.Empty:
            pass

        now_ms = int(time.monotonic() * 1000) - start_ms
        buf = render_frame(state, fonts, now_ms)
        surf = buf_to_pygame_surface(buf, scale)
        screen.blit(surf, (0, 0))
        pygame.display.flip()
        clock.tick(max(1, args.fps))


if __name__ == "__main__":
    main()
