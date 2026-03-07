from __future__ import annotations

import time
from typing import Any

try:
    import smbus2  # type: ignore
except ImportError:  # pragma: no cover - hardware-specific dependency
    smbus2 = None


class AudioRouterAdapter:
    """Routes audio sources through an external audio switch/processor.

    PT2314 is treated as an externally-configured command sink. Command bytes
    are provided per source in config so board variants can be supported
    without code changes.
    """

    def __init__(self, config: dict[str, Any]) -> None:
        self._cfg = config
        self._chip = str(self._cfg.get("chip", "pt2314")).strip().lower()
        self._protocol = str(self._cfg.get("protocol", "i2c")).strip().lower()
        self._status = "disabled"
        self._active_source = ""
        self._bus = None

    def start(self) -> None:
        if not bool(self._cfg.get("enabled", False)):
            self._status = "disabled"
            return
        if self._chip != "pt2314":
            self._status = f"unsupported-chip:{self._chip}"
            return
        if self._protocol != "i2c":
            self._status = f"unsupported-protocol:{self._protocol}"
            return
        if smbus2 is None:
            self._status = "i2c-unavailable"
            return
        try:
            bus_id = int(self._cfg.get("i2c_bus", 1))
            self._bus = smbus2.SMBus(bus_id)
            default_source = str(self._cfg.get("default_source", "")).strip()
            if default_source:
                self.select_source(default_source)
            else:
                self._status = "ready:no-default-source"
        except Exception as exc:
            self._status = f"error:{exc}"
            self._bus = None

    def stop(self) -> None:
        if self._bus is not None:
            try:
                self._bus.close()
            except Exception:
                pass
        self._bus = None
        if bool(self._cfg.get("enabled", False)):
            self._status = "stopped"
        else:
            self._status = "disabled"

    def status(self) -> str:
        if not bool(self._cfg.get("enabled", False)):
            return "disabled"
        return self._status

    def active_source(self) -> str:
        return self._active_source or "-"

    def select_source(self, source_id: str) -> str:
        if not bool(self._cfg.get("enabled", False)):
            self._status = "disabled"
            return self._status
        source = str(source_id or "").strip()
        if not source:
            self._status = "invalid-source"
            return self._status
        if self._bus is None:
            self._status = "not-started"
            return self._status

        source_cfg = self._resolve_source_config(source)
        if not isinstance(source_cfg, dict):
            self._status = f"unknown-source:{source}"
            return self._status
        raw_cmds = source_cfg.get("command_bytes", [])
        if not isinstance(raw_cmds, list) or len(raw_cmds) < 1:
            self._status = f"invalid-command:{source}"
            return self._status

        commands: list[int] = []
        for value in raw_cmds:
            if isinstance(value, str):
                commands.append(int(value, 0) & 0xFF)
            else:
                commands.append(int(value) & 0xFF)
        try:
            address = int(self._cfg.get("address", 0x44))
            for command in commands:
                self._bus.write_byte(address, command)
                time.sleep(max(0.0, float(self._cfg.get("inter_command_delay_s", 0.003))))
            settle_s = max(0.0, float(self._cfg.get("settle_ms", 10.0)) / 1000.0)
            if settle_s > 0.0:
                time.sleep(settle_s)
            self._active_source = source
            self._status = f"active:{source}"
        except Exception as exc:
            self._status = f"error:{exc}"
        return self._status

    def _resolve_source_config(self, source_id: str) -> dict[str, Any] | None:
        raw_sources = self._cfg.get("sources", {})
        if not isinstance(raw_sources, dict):
            return None
        value = raw_sources.get(source_id)
        return value if isinstance(value, dict) else None
