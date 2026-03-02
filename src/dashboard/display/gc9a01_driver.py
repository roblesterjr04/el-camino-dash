from __future__ import annotations

import logging
import time
from typing import Iterable

import numpy as np
from PIL import Image

try:
    import spidev
except Exception:  # pragma: no cover - import-time dependency availability
    spidev = None

try:
    import RPi.GPIO as GPIO
except Exception:  # pragma: no cover - import-time dependency availability
    GPIO = None


LOG = logging.getLogger(__name__)


class GC9A01:
    def __init__(
        self,
        spi_bus: int,
        spi_device: int,
        dc_pin: int,
        rst_pin: int,
        speed_hz: int = 8_000_000,
        width: int = 240,
        height: int = 240,
        rotation: int = 0,
        madctl: int | None = None,
        colmod: int = 0x55,
    ) -> None:
        """
        spi_bus: 1 (SPI1)
        spi_device: 0, 1, or 2 (chip select)
        dc_pin: GPIO number for data/command
        rst_pin: GPIO number for reset
        """
        self.spi_bus = int(spi_bus)
        self.spi_device = int(spi_device)
        self.dc_pin = int(dc_pin)
        self.rst_pin = int(rst_pin)
        self.speed_hz = int(speed_hz)
        self.width = int(width)
        self.height = int(height)
        self.rotation = int(rotation)
        self.madctl = self._resolve_madctl(madctl, self.rotation)
        self.colmod = int(colmod) & 0xFF
        self.spi = None

    def _resolve_madctl(self, madctl: int | None, rotation: int) -> int:
        if madctl is not None:
            return int(madctl) & 0xFF
        lookup = {
            0: 0x08,    # BGR
            90: 0x68,   # MV + MX + BGR
            180: 0xC8,  # MX + MY + BGR
            270: 0xA8,  # MV + MY + BGR
        }
        return lookup.get(int(rotation) % 360, 0x08)

    def _require_runtime_deps(self) -> None:
        if spidev is None:
            raise RuntimeError("spidev module is not available")
        if GPIO is None:
            raise RuntimeError("RPi.GPIO module is not available")

    def init(self) -> None:
        self._require_runtime_deps()
        assert GPIO is not None
        assert spidev is not None

        GPIO.setwarnings(False)
        GPIO.setmode(GPIO.BCM)
        GPIO.setup(self.dc_pin, GPIO.OUT, initial=GPIO.LOW)
        GPIO.setup(self.rst_pin, GPIO.OUT, initial=GPIO.HIGH)

        self.spi = spidev.SpiDev()
        self.spi.open(self.spi_bus, self.spi_device)
        self.spi.max_speed_hz = self.speed_hz
        self.spi.mode = 0
        self.spi.bits_per_word = 8

        self._hardware_reset()
        self._run_init_sequence()

    def _cmd(self, command: int, data: list[int] | None = None) -> None:
        self.send_command(command)
        if data:
            self.send_data(data)

    def _run_init_sequence(self) -> None:
        self.send_command(0x01)
        time.sleep(0.15)

        self.send_command(0xEF)
        self.send_command(0xEB)
        self.send_data([0x14])

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

        self.send_command(0x21)
        self.send_command(0x11)
        time.sleep(0.12)
        self.send_command(0x29)
        time.sleep(0.02)

    def _hardware_reset(self) -> None:
        assert GPIO is not None
        GPIO.output(self.rst_pin, GPIO.LOW)
        time.sleep(0.1)
        GPIO.output(self.rst_pin, GPIO.HIGH)
        time.sleep(0.1)

    def send_command(self, cmd: int) -> None:
        if self.spi is None:
            raise RuntimeError("SPI device not initialized")
        assert GPIO is not None
        GPIO.output(self.dc_pin, GPIO.LOW)
        self.spi.xfer2([int(cmd) & 0xFF])

    def send_data(self, data: bytes | bytearray | memoryview | Iterable[int]) -> None:
        if self.spi is None:
            raise RuntimeError("SPI device not initialized")
        assert GPIO is not None
        GPIO.output(self.dc_pin, GPIO.HIGH)
        if isinstance(data, (bytes, bytearray, memoryview)):
            payload = bytes(data)
        else:
            payload = bytes(int(v) & 0xFF for v in data)
        if payload:
            self.spi.writebytes2(payload)

    def set_window(self, x0: int, y0: int, x1: int, y1: int) -> None:
        self.send_command(0x2A)  # Column address set
        self.send_data(
            [
                (x0 >> 8) & 0xFF,
                x0 & 0xFF,
                (x1 >> 8) & 0xFF,
                x1 & 0xFF,
            ]
        )
        self.send_command(0x2B)  # Row address set
        self.send_data(
            [
                (y0 >> 8) & 0xFF,
                y0 & 0xFF,
                (y1 >> 8) & 0xFF,
                y1 & 0xFF,
            ]
        )

    def write_frame(self, pil_image: Image.Image) -> None:
        if self.spi is None:
            raise RuntimeError("SPI device not initialized")

        frame = pil_image.convert("RGB")
        if frame.size != (self.width, self.height):
            frame = frame.resize((self.width, self.height), resample=Image.BILINEAR)

        arr = np.asarray(frame, dtype=np.uint8)
        r = (arr[:, :, 0] >> 3).astype(np.uint16)
        g = (arr[:, :, 1] >> 2).astype(np.uint16)
        b = (arr[:, :, 2] >> 3).astype(np.uint16)
        rgb565 = (r << 11) | (g << 5) | b
        data = rgb565.byteswap().tobytes()

        self.set_window(0, 0, self.width - 1, self.height - 1)
        self.send_command(0x2C)  # Memory write
        chunk_size = 4096
        for idx in range(0, len(data), chunk_size):
            self.send_data(data[idx : idx + chunk_size])

    def cleanup(self) -> None:
        if self.spi is not None:
            try:
                self.spi.close()
            except Exception as exc:  # pragma: no cover - hardware dependent
                LOG.debug("SPI close failed: %s", exc)
            self.spi = None
        if GPIO is not None:
            try:
                GPIO.cleanup([self.dc_pin, self.rst_pin])
            except Exception as exc:  # pragma: no cover - hardware dependent
                LOG.debug("GPIO cleanup failed: %s", exc)
