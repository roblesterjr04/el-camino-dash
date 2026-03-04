from __future__ import annotations

import math
import time
from typing import Any

import pygame

from dashboard.screens.layout import DisplayLayout


class WarningOverlayScreen:
    def __init__(self, layout: DisplayLayout, config: dict[str, Any]) -> None:
        self.layout = layout
        self.config = config
        font_name = config["themes"]["font_name"]
        self._title_font = pygame.font.Font(font_name, max(18, layout.height // 10))
        self._detail_font = pygame.font.Font(font_name, max(13, layout.height // 20))

    def render(self, surface: pygame.Surface, active_count: int, headline: str) -> None:
        pulse = 0.5 + (0.5 * math.sin(time.monotonic() * 2.6))
        shade_alpha = 56 + int(32 * pulse)
        glow_alpha = 70 + int(45 * pulse)

        shade = pygame.Surface(self.layout.size, pygame.SRCALPHA)
        shade.fill((255, 140, 0, shade_alpha))
        surface.blit(shade, (0, 0))

        center = (self.layout.width // 2, self.layout.height // 2)
        radius = int(min(self.layout.width, self.layout.height) * 0.30)
        tri_h = int(radius * 1.72)
        top = (center[0], center[1] - (tri_h // 2))
        left = (center[0] - radius, center[1] + (tri_h // 2))
        right = (center[0] + radius, center[1] + (tri_h // 2))
        triangle = [top, left, right]

        glow = pygame.Surface(self.layout.size, pygame.SRCALPHA)
        pygame.draw.polygon(glow, (255, 186, 58, glow_alpha), triangle)
        surface.blit(glow, (0, 0))
        pygame.draw.polygon(surface, (255, 210, 84), triangle, width=max(3, radius // 11))

        ex_height = int(tri_h * 0.30)
        ex_width = max(8, radius // 8)
        ex_y_top = center[1] - int(tri_h * 0.20)
        ex_rect = pygame.Rect(center[0] - (ex_width // 2), ex_y_top, ex_width, ex_height)
        pygame.draw.rect(surface, (35, 24, 8), ex_rect, border_radius=max(2, ex_width // 3))
        pygame.draw.circle(surface, (35, 24, 8), (center[0], center[1] + int(tri_h * 0.20)), max(4, ex_width // 2))

        detail_text = f"{headline} ({active_count})" if active_count > 0 else headline
        detail = self._detail_font.render(detail_text[:48], True, (255, 240, 188))
        detail_rect = detail.get_rect(center=(center[0], int(self.layout.height * 0.86)))
        surface.blit(detail, detail_rect)
