from __future__ import annotations

import subprocess
from typing import Any


class NavitAdapter:
    def __init__(self, config: dict[str, Any]) -> None:
        self._cfg = config
        self._proc: subprocess.Popen[bytes] | None = None

    def start(self) -> None:
        if not self._cfg.get("enabled", False):
            return
        if self._proc and self._proc.poll() is None:
            return
        cmd = [self._cfg.get("executable", "navit"), *self._cfg.get("args", [])]
        try:
            self._proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError:
            self._proc = None

    def stop(self) -> None:
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()

    def status(self) -> str:
        if not self._cfg.get("enabled", False):
            return "disabled"
        if self._proc and self._proc.poll() is None:
            return "running"
        return "stopped"
