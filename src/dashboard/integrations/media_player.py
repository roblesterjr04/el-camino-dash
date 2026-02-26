from __future__ import annotations

from pathlib import Path
from typing import Any

import pygame


class MediaPlayer:
    def __init__(self, config: dict[str, Any]) -> None:
        self._cfg = config
        self._loaded = False
        self._tracks = self._scan_tracks()
        self._track_index = 0
        self._paused = False
        startup_volume = float(self._cfg.get("startup_volume", 0.5))
        self.set_volume(startup_volume)

    def _scan_tracks(self) -> list[Path]:
        music_root = Path(self._cfg.get("music_root", "media"))
        if not music_root.exists():
            return []
        tracks: list[Path] = []
        for ext in ("*.mp3", "*.ogg", "*.wav"):
            tracks.extend(sorted(music_root.glob(ext)))
        return tracks

    def _ensure_loaded(self) -> None:
        if self._loaded or not self._tracks:
            return
        pygame.mixer.music.load(str(self._tracks[self._track_index]))
        self._loaded = True

    def play(self) -> None:
        if not self._cfg.get("enabled", False):
            return
        self._ensure_loaded()
        if self._loaded:
            pygame.mixer.music.play()
            self._paused = False

    def pause(self) -> None:
        pygame.mixer.music.pause()
        self._paused = True

    def stop(self) -> None:
        pygame.mixer.music.stop()
        self._paused = False

    def toggle_play_pause(self) -> None:
        if not self._cfg.get("enabled", False):
            return
        if self._paused:
            pygame.mixer.music.unpause()
            self._paused = False
            return
        if pygame.mixer.music.get_busy():
            self.pause()
            return
        self.play()

    def next_track(self) -> None:
        if not self._tracks:
            return
        self._track_index = (self._track_index + 1) % len(self._tracks)
        self._loaded = False
        self.play()

    def set_volume(self, volume: float) -> None:
        pygame.mixer.music.set_volume(max(0.0, min(1.0, volume)))

    def status(self) -> str:
        if not self._cfg.get("enabled", False):
            return "disabled"
        if self._paused:
            return "paused"
        if pygame.mixer.music.get_busy():
            return "playing"
        return "idle"
