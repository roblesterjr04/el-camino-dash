from __future__ import annotations

from typing import Any

try:
    import spidev  # type: ignore
except ImportError:  # pragma: no cover
    spidev = None


class AnalogAdcInput:
    def __init__(self, spec: dict[str, Any]) -> None:
        self.spec = spec
        self._spi = None
        if spidev and spec.get("enabled", False) and spec.get("adc_type") == "mcp3008":
            self._spi = spidev.SpiDev()
            self._spi.open(int(spec.get("spi_bus", 0)), int(spec.get("spi_device", 0)))
            self._spi.max_speed_hz = 1_000_000

    def _read_raw(self) -> int:
        if not self._spi:
            return int(self.spec.get("min_raw", 0))
        channel = int(self.spec["channel"])
        request = [1, (8 + channel) << 4, 0]
        response = self._spi.xfer2(request)
        return ((response[1] & 3) << 8) + response[2]

    def poll(self) -> dict[str, float]:
        raw = self._read_raw()
        min_raw = float(self.spec["min_raw"])
        max_raw = float(self.spec["max_raw"])
        out_min = float(self.spec["scale_min"])
        out_max = float(self.spec["scale_max"])
        ratio = 0.0 if max_raw <= min_raw else (raw - min_raw) / (max_raw - min_raw)
        ratio = max(0.0, min(1.0, ratio))
        value = out_min + ((out_max - out_min) * ratio)
        return {f"analog_inputs.{self.spec['id']}": value}
