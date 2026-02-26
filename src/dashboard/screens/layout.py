from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class DisplayLayout:
    display_id: str
    role: str
    shape: str
    width: int
    height: int
    x: int
    y: int
    fps: int
    gauge_id: str | None = None

    @property
    def size(self) -> tuple[int, int]:
        return (self.width, self.height)
