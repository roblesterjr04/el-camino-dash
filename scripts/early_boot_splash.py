#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw
import RPi.GPIO as GPIO
import spidev


DISPLAYS = [
    {"spi_bus": 1, "spi_device": 0, "dc": 24, "rst": 27, "label": "Gauge 1"},
    {"spi_bus": 1, "spi_device": 1, "dc": 23, "rst": 26, "label": "Gauge 2"},
    {"spi_bus": 1, "spi_device": 2, "dc": 22, "rst": 25, "label": "Gauge 3"},
]


def _build_pattern(kind: str) -> Image.Image:
    image = Image.new("RGB", (240, 240), (0, 0, 0))
    draw = ImageDraw.Draw(image)
    if kind == "red":
        draw.rectangle([0, 0, 239, 239], fill=(255, 0, 0))
    elif kind == "green":
        draw.rectangle([0, 0, 239, 239], fill=(0, 255, 0))
    elif kind == "blue":
        draw.rectangle([0, 0, 239, 239], fill=(0, 0, 255))
    elif kind == "white":
        draw.rectangle([0, 0, 239, 239], fill=(255, 255, 255))
    elif kind == "bars":
        band_w = 240 // 6
        colors = [
            (255, 0, 0),
            (255, 255, 0),
            (0, 255, 0),
            (0, 255, 255),
            (0, 0, 255),
            (255, 0, 255),
        ]
        for idx, color in enumerate(colors):
            x0 = idx * band_w
            x1 = 239 if idx == (len(colors) - 1) else ((idx + 1) * band_w) - 1
            draw.rectangle([x0, 0, x1, 239], fill=color)
    return image


class GC9A01:
    def __init__(
        self,
        spi_bus: int,
        spi_device: int,
        dc_pin: int,
        rst_pin: int,
        speed_hz: int = 8_000_000,
        madctl: int = 0x08,
        colmod: int = 0x55,
    ) -> None:
        self.spi_bus = int(spi_bus)
        self.spi_device = int(spi_device)
        self.dc_pin = int(dc_pin)
        self.rst_pin = int(rst_pin)
        self.speed_hz = int(speed_hz)
        self.madctl = int(madctl) & 0xFF
        self.colmod = int(colmod) & 0xFF
        self.spi: spidev.SpiDev | None = None

    def init(self) -> None:
        GPIO.setup(self.dc_pin, GPIO.OUT, initial=GPIO.LOW)
        GPIO.setup(self.rst_pin, GPIO.OUT, initial=GPIO.HIGH)

        self.spi = spidev.SpiDev()
        self.spi.open(self.spi_bus, self.spi_device)
        self.spi.max_speed_hz = self.speed_hz
        self.spi.mode = 0
        self.spi.bits_per_word = 8

        GPIO.output(self.rst_pin, GPIO.LOW)
        _sleep_ms(100)
        GPIO.output(self.rst_pin, GPIO.HIGH)
        _sleep_ms(100)

        self._run_init_sequence()

    def _cmd(self, command: int, data: list[int] | None = None) -> None:
        self.send_command(command)
        if data:
            self.send_data(bytes(data))

    def _run_init_sequence(self) -> None:
        # Full GC9A01 bring-up sequence (derived from known-good vendor init tables).
        self.send_command(0x01)  # Software reset
        _sleep_ms(150)

        self.send_command(0xEF)
        self.send_command(0xEB)
        self.send_data(bytes([0x14]))

        self.send_command(0xFE)
        self.send_command(0xEF)

        self._cmd(0xEB, [0x14])
        self._cmd(0x84, [0x40])
        self._cmd(0x85, [0xFF])
        self._cmd(0x86, [0xFF])
        self._cmd(0x87, [0xFF])
        self._cmd(0x88, [0x0A])
        self._cmd(0x89, [0x21])
        self._cmd(0x8A, [0x00])
        self._cmd(0x8B, [0x80])
        self._cmd(0x8C, [0x01])
        self._cmd(0x8D, [0x01])
        self._cmd(0x8E, [0xFF])
        self._cmd(0x8F, [0xFF])

        self._cmd(0xB6, [0x00, 0x20])
        self._cmd(0x36, [self.madctl])
        self._cmd(0x3A, [self.colmod])
        self._cmd(0x90, [0x08, 0x08, 0x08, 0x08])
        self._cmd(0xBD, [0x06])
        self._cmd(0xBC, [0x00])
        self._cmd(0xFF, [0x60, 0x01, 0x04])
        self._cmd(0xC3, [0x13])
        self._cmd(0xC4, [0x13])
        self._cmd(0xC9, [0x22])
        self._cmd(0xBE, [0x11])
        self._cmd(0xE1, [0x10, 0x0E])
        self._cmd(0xDF, [0x21, 0x0C, 0x02])

        self._cmd(0xF0, [0x45, 0x09, 0x08, 0x08, 0x26, 0x2A])
        self._cmd(0xF1, [0x43, 0x70, 0x72, 0x36, 0x37, 0x6F])
        self._cmd(0xF2, [0x45, 0x09, 0x08, 0x08, 0x26, 0x2A])
        self._cmd(0xF3, [0x43, 0x70, 0x72, 0x36, 0x37, 0x6F])

        self._cmd(0xED, [0x1B, 0x0B])
        self._cmd(0xAE, [0x77])
        self._cmd(0xCD, [0x63])

        self._cmd(0x70, [0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03])
        self._cmd(0xE8, [0x34])
        self._cmd(0x62, [0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70])
        self._cmd(0x63, [0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70])
        self._cmd(0x64, [0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07])
        self._cmd(0x66, [0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00])
        self._cmd(0x67, [0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98])
        self._cmd(0x74, [0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00])
        self._cmd(0x98, [0x3E, 0x07])

        self.send_command(0x21)  # Inversion on
        self.send_command(0x11)  # Sleep out
        _sleep_ms(120)
        self.send_command(0x29)  # Display on
        _sleep_ms(20)

    def send_command(self, cmd: int) -> None:
        if self.spi is None:
            raise RuntimeError("SPI not initialized")
        GPIO.output(self.dc_pin, GPIO.LOW)
        self.spi.xfer2([cmd & 0xFF])

    def send_data(self, data: bytes) -> None:
        if self.spi is None:
            raise RuntimeError("SPI not initialized")
        GPIO.output(self.dc_pin, GPIO.HIGH)
        if data:
            self.spi.writebytes2(data)

    def set_window(self, x0: int, y0: int, x1: int, y1: int) -> None:
        self.send_command(0x2A)
        self.send_data(bytes([(x0 >> 8) & 0xFF, x0 & 0xFF, (x1 >> 8) & 0xFF, x1 & 0xFF]))
        self.send_command(0x2B)
        self.send_data(bytes([(y0 >> 8) & 0xFF, y0 & 0xFF, (y1 >> 8) & 0xFF, y1 & 0xFF]))

    def write_frame(self, image: Image.Image) -> None:
        frame = image.convert("RGB").resize((240, 240), resample=Image.BILINEAR)
        rgb = frame.tobytes()
        data = bytearray((240 * 240) * 2)
        out_idx = 0
        for i in range(0, len(rgb), 3):
            r = rgb[i]
            g = rgb[i + 1]
            b = rgb[i + 2]
            value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            data[out_idx] = (value >> 8) & 0xFF
            data[out_idx + 1] = value & 0xFF
            out_idx += 2

        self.set_window(0, 0, 239, 239)
        self.send_command(0x2C)
        for i in range(0, len(data), 4096):
            self.send_data(data[i : i + 4096])

    def cleanup(self) -> None:
        if self.spi is not None:
            self.spi.close()
            self.spi = None


def _sleep_ms(ms: int) -> None:
    import time

    time.sleep(ms / 1000.0)


def _load_masked_image(image_path: Path) -> Image.Image:
    src = Image.open(image_path).convert("RGB").resize((240, 240), resample=Image.BILINEAR)
    return _apply_round_mask(src)


def _apply_round_mask(src: Image.Image) -> Image.Image:
    mask = Image.new("L", (240, 240), 0)
    draw = ImageDraw.Draw(mask)
    draw.ellipse([0, 0, 239, 239], fill=255)
    black = Image.new("RGB", (240, 240), (0, 0, 0))
    return Image.composite(src, black, mask)


def main() -> int:
    parser = argparse.ArgumentParser(description="Early SPI boot splash for 3x GC9A01 displays")
    parser.add_argument("--image", default="assets/bootscreen/boot.png", help="Path to splash image")
    parser.add_argument("--speed-hz", type=int, default=8_000_000, help="SPI speed in Hz")
    parser.add_argument(
        "--madctl",
        type=lambda value: int(value, 0),
        default=0x08,
        help="MADCTL register value (e.g. 0x08 BGR, 0x28 BGR+MV)",
    )
    parser.add_argument(
        "--colmod",
        type=lambda value: int(value, 0),
        default=0x55,
        help="COLMOD pixel format (0x55=16-bit recommended, 0x05 on some modules)",
    )
    parser.add_argument(
        "--pattern",
        choices=("image", "bars", "red", "green", "blue", "white"),
        default="image",
        help="Render a built-in pattern instead of loading image",
    )
    parser.add_argument(
        "--display-index",
        type=int,
        action="append",
        help="Optional 0-based display index to target (repeatable)",
    )
    args = parser.parse_args()

    if args.pattern == "image":
        image_path = Path(args.image)
        if not image_path.exists():
            print(f"Image not found: {image_path}", file=sys.stderr)
            return 2
        image = _load_masked_image(image_path)
    else:
        image = _apply_round_mask(_build_pattern(args.pattern))

    GPIO.setwarnings(False)
    GPIO.setmode(GPIO.BCM)
    drivers: list[GC9A01] = []
    selected = set(args.display_index or [])

    try:
        for idx, spec in enumerate(DISPLAYS):
            if selected and idx not in selected:
                continue
            drv = GC9A01(
                spi_bus=spec["spi_bus"],
                spi_device=spec["spi_device"],
                dc_pin=spec["dc"],
                rst_pin=spec["rst"],
                speed_hz=args.speed_hz,
                madctl=args.madctl,
                colmod=args.colmod,
            )
            print(
                f"init display[{idx}] {spec['label']} spi={spec['spi_bus']}.{spec['spi_device']} "
                f"dc={spec['dc']} rst={spec['rst']} speed={args.speed_hz} "
                f"madctl=0x{args.madctl:02X} colmod=0x{args.colmod:02X}"
            )
            drv.init()
            # Pre-clear to avoid residual artifacts from boot ROM/power-up states.
            drv.write_frame(Image.new("RGB", (240, 240), (0, 0, 0)))
            drv.write_frame(image)
            drivers.append(drv)
    finally:
        for drv in drivers:
            drv.cleanup()
        GPIO.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
