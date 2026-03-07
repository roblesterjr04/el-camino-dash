from __future__ import annotations

from typing import Any

import pygame

from dashboard.screens.layout import DisplayLayout


class WarningOverlayScreen:
    def __init__(self, layout: DisplayLayout, config: dict[str, Any]) -> None:
        self.layout = layout
        self.config = config
        font_name = config["themes"]["font_name"]
        self._detail_font = pygame.font.Font(font_name, max(13, layout.height // 20))
        self._mode = str(config.get("system", {}).get("alert_overlay_mode", "full")).strip().lower()
        if self._mode not in {"full", "lower_third"}:
            self._mode = "full"

    def render(self, surface: pygame.Surface, active_count: int, headline: str) -> None:
        if self._mode == "lower_third":
            self._render_lower_third(surface, active_count=active_count, headline=headline)
            return
        self._render_full(surface, active_count=active_count, headline=headline)

    def _render_full(self, surface: pygame.Surface, active_count: int, headline: str) -> None:
        shade = pygame.Surface(self.layout.size, pygame.SRCALPHA)
        shade.fill((0, 0, 0, 110))
        surface.blit(shade, (0, 0))

        center = (self.layout.width // 2, self.layout.height // 2)
        radius = int(min(self.layout.width, self.layout.height) * 0.31)
        tri_h = int(radius * 1.75)
        top = (center[0], center[1] - (tri_h // 2))
        left = (center[0] - radius, center[1] + (tri_h // 2))
        right = (center[0] + radius, center[1] + (tri_h // 2))
        triangle = [top, left, right]

        pygame.draw.polygon(surface, (255, 225, 86), triangle)
        pygame.draw.polygon(surface, (12, 12, 12), triangle, width=max(4, radius // 9))

        ex_height = int(tri_h * 0.32)
        ex_width = max(10, radius // 7)
        ex_y_top = center[1] - int(tri_h * 0.20)
        ex_rect = pygame.Rect(center[0] - (ex_width // 2), ex_y_top, ex_width, ex_height)
        pygame.draw.rect(surface, (8, 8, 8), ex_rect, border_radius=max(2, ex_width // 3))
        pygame.draw.circle(surface, (8, 8, 8), (center[0], center[1] + int(tri_h * 0.22)), max(4, ex_width // 2))

        detail_text = f"{headline} ({active_count})" if active_count > 0 else headline
        detail = self._detail_font.render(detail_text[:48], True, (255, 248, 214))
        detail_rect = detail.get_rect(center=(center[0], int(self.layout.height * 0.88)))
        surface.blit(detail, detail_rect)

    def _render_lower_third(self, surface: pygame.Surface, active_count: int, headline: str) -> None:
        panel_h = max(56, int(self.layout.height * 0.27))
        panel = pygame.Rect(0, self.layout.height - panel_h, self.layout.width, panel_h)
        pygame.draw.rect(surface, (0, 0, 0, 210), panel)
        pygame.draw.line(
            surface,
            (255, 228, 96),
            (panel.left, panel.top + 1),
            (panel.right, panel.top + 1),
            width=max(2, panel_h // 18),
        )

        tri_size = max(18, int(panel_h * 0.42))
        cx = panel.left + max(28, int(panel_h * 0.52))
        cy = panel.centery
        tri = [(cx, cy - tri_size), (cx - tri_size, cy + tri_size), (cx + tri_size, cy + tri_size)]
        pygame.draw.polygon(surface, (255, 225, 86), tri)
        pygame.draw.polygon(surface, (10, 10, 10), tri, width=max(3, tri_size // 5))
        ex_h = int(tri_size * 0.95)
        ex_w = max(4, tri_size // 4)
        pygame.draw.rect(
            surface,
            (8, 8, 8),
            pygame.Rect(cx - (ex_w // 2), cy - int(tri_size * 0.55), ex_w, ex_h),
            border_radius=max(1, ex_w // 3),
        )
        pygame.draw.circle(surface, (8, 8, 8), (cx, cy + int(tri_size * 0.62)), max(2, ex_w // 2))

        detail_text = f"{headline} ({active_count})" if active_count > 0 else headline
        detail = self._detail_font.render(detail_text[:56], True, (255, 248, 214))
        detail_rect = detail.get_rect(midleft=(cx + tri_size + 16, cy))
        surface.blit(detail, detail_rect)
