from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pygame

from dashboard.screens.layout import DisplayLayout


class MapScreen:
    def __init__(self, layout: DisplayLayout, config: dict[str, Any]) -> None:
        self.layout = layout
        self.config = config
        self._cached_surface: pygame.Surface | None = None
        self._cached_path: Path | None = None
        self._font = pygame.font.Font(config["themes"]["font_name"], max(14, layout.height // 22))
        self._theme_data = self._load_theme_data()

    def _load_theme_data(self) -> dict[str, Any]:
        theme_root = Path(self.config["themes"]["theme_root"])
        theme_name = self.config["themes"]["active"]
        theme_path = theme_root / theme_name / "theme.json"
        if not theme_path.exists():
            return {}
        try:
            return json.loads(theme_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return {}

    def _resolve_map_path(self) -> Path:
        system_cfg = self.config.get("system", {})
        sim_mode = str(system_cfg.get("mode", "")).lower() == "simulation"
        key = "simulation_map_image_path" if sim_mode else "map_image_path"
        raw = str(system_cfg.get(key, "assets/maps/testmap.png"))
        candidate = Path(raw)
        if candidate.is_absolute():
            return candidate
        if raw.startswith("/"):
            return Path(raw.lstrip("/"))
        return candidate

    def _load_map_surface(self) -> pygame.Surface | None:
        map_path = self._resolve_map_path()
        if self._cached_surface is not None and self._cached_path == map_path:
            return self._cached_surface
        if not map_path.exists():
            self._cached_surface = None
            self._cached_path = map_path
            return None
        loaded = pygame.image.load(str(map_path))
        try:
            self._cached_surface = loaded.convert_alpha()
        except pygame.error:
            self._cached_surface = loaded
        self._cached_path = map_path
        return self._cached_surface

    def _map_view_settings(self) -> dict[str, Any]:
        cfg = self._theme_data.get("map_view", {})
        if not isinstance(cfg, dict):
            return {}
        merged: dict[str, Any] = {}
        default_cfg = cfg.get("_default", {})
        if isinstance(default_cfg, dict):
            merged.update(default_cfg)
        by_display = cfg.get(self.layout.display_id, {})
        if isinstance(by_display, dict):
            merged.update(by_display)
        return merged

    def render(self, surface: pygame.Surface, values: dict[str, float], status: dict[str, str]) -> None:
        del values
        del status
        surface.fill((0, 0, 0, 0))
        map_surface = self._load_map_surface()
        if not map_surface:
            message = "Map image missing"
            text = self._font.render(message, True, (230, 230, 230))
            rect = text.get_rect(center=(self.layout.width // 2, self.layout.height // 2))
            surface.blit(text, rect.topleft)
            return

        view_cfg = self._map_view_settings()
        scale = max(0.1, min(1.5, float(view_cfg.get("scale", 1.0))))
        view_w = max(1, int(self.layout.width * scale))
        view_h = max(1, int(self.layout.height * scale))
        center = view_cfg.get("center_ratio", [0.5, 0.5])
        if not isinstance(center, list) or len(center) != 2:
            center = [0.5, 0.5]
        center_x = int(self.layout.width * float(center[0]))
        center_y = int(self.layout.height * float(center[1]))
        offset_x = int(view_cfg.get("offset_x_px", 0))
        offset_y = int(view_cfg.get("offset_y_px", 0))
        offset_pair = view_cfg.get("offset_px")
        if isinstance(offset_pair, list) and len(offset_pair) == 2:
            offset_x = int(offset_pair[0])
            offset_y = int(offset_pair[1])
        center_x += offset_x
        center_y += offset_y
        view_x = center_x - (view_w // 2)
        view_y = center_y - (view_h // 2)
        view_rect = pygame.Rect(view_x, view_y, view_w, view_h)

        src_w, src_h = map_surface.get_size()
        if src_w <= 0 or src_h <= 0:
            return
        fit_scale = min(view_rect.width / src_w, view_rect.height / src_h)
        draw_w = max(1, int(src_w * fit_scale))
        draw_h = max(1, int(src_h * fit_scale))
        resized = pygame.transform.smoothscale(map_surface, (draw_w, draw_h))
        x = view_rect.x + ((view_rect.width - draw_w) // 2)
        y = view_rect.y + ((view_rect.height - draw_h) // 2)
        map_layer = pygame.Surface((self.layout.width, self.layout.height), pygame.SRCALPHA)
        map_layer.blit(resized, (x, y))

        if self.layout.shape == "round":
            diameter = min(view_rect.width, view_rect.height)
            radius = diameter // 2
            mask = pygame.Surface((self.layout.width, self.layout.height), pygame.SRCALPHA)
            pygame.draw.circle(mask, (255, 255, 255, 255), (center_x, center_y), radius)
            map_layer.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(map_layer, (0, 0))
