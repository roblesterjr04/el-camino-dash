from __future__ import annotations

from typing import Any


class BluetoothMetadataAdapter:
    def __init__(self, config: dict[str, Any]) -> None:
        self._cfg = config

    def status(self) -> str:
        if not self._cfg.get("enabled", False):
            return "disabled"
        return f"enabled:{self._cfg.get('provider', 'unknown')}"
