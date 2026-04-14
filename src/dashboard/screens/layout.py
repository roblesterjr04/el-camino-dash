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
    monitor_index: int | None = None
    windowed: bool = True
    fullscreen: bool = False
    # None = inherit system.display_rotation_deg (0 or 180).
    view_rotation_deg: int | None = None

    @property
    def size(self) -> tuple[int, int]:
        return (self.width, self.height)
