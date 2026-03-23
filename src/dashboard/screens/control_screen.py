from __future__ import annotations

from pathlib import Path
from typing import Any
from typing import Callable

import pygame

from dashboard.screens.control_menu import ControlMenuController
from dashboard.screens.control_menu import MenuRenderModel
from dashboard.screens.control_menu import MenuSchemaError
from dashboard.screens.control_menu import load_menu_schema
from dashboard.screens.layout import DisplayLayout


class ControlScreen:
    def __init__(
        self,
        layout: DisplayLayout,
        config: dict[str, Any],
        config_path: Path,
        command_registry: dict[str, Callable[[dict[str, Any]], str | None]],
        on_runtime_refresh: Callable[[], None] | None = None,
    ) -> None:
        self.layout = layout
        self.config = config
        self._font = pygame.font.Font(config["themes"]["font_name"], max(18, layout.height // 14))
        self._small_font = pygame.font.Font(config["themes"]["font_name"], max(14, layout.height // 22))
        self._tiny_font = pygame.font.Font(config["themes"]["font_name"], max(12, layout.height // 30))
        self._menu_error = ""
        self._menu: ControlMenuController | None = None
        schema_path = self.config["system"].get("menu_schema_path", "config/menu_schema.yaml")
        try:
            schema = load_menu_schema(
                schema_path=schema_path,
                config=config,
                command_ids={"config.save", "config.reload", *command_registry.keys()},
            )
            self._menu = ControlMenuController(
                schema=schema,
                config=config,
                config_path=config_path,
                command_registry=command_registry,
                on_runtime_refresh=on_runtime_refresh,
            )
        except (MenuSchemaError, OSError) as exc:
            self._menu_error = str(exc)

    def render(self, surface: pygame.Surface, values: dict[str, float], status: dict[str, str]) -> None:
        self._paint_background(surface)
        if not self._menu:
            header = self._font.render("Menu schema error", True, (255, 180, 160))
            surface.blit(header, (20, 14))
            err = self._tiny_font.render(self._menu_error[:100], True, (255, 120, 100))
            surface.blit(err, (20, 58))
            return

        self._menu.process_input(values=values, status=status)
        model = self._menu.render_model(status=status)
        header = self._font.render(model.title, True, (238, 242, 246))
        surface.blit(header, (20, 14))
        pygame.draw.line(surface, (50, 74, 98), (20, 52), (self.layout.width - 20, 52), 2)

        if model.page_id == "home":
            self._render_home_cards(surface, model)
        else:
            self._render_list(surface, model)

        footer_text = self._tiny_font.render(model.footer, True, (150, 168, 190))
        surface.blit(footer_text, (20, self.layout.height - 26))
        if model.message:
            msg_text = self._tiny_font.render(model.message, True, (255, 214, 120))
            surface.blit(msg_text, (20, self.layout.height - 48))

    def _paint_background(self, surface: pygame.Surface) -> None:
        top = (18, 24, 36)
        bottom = (10, 14, 22)
        for y in range(self.layout.height):
            mix = y / max(1, self.layout.height - 1)
            color = (
                int(top[0] + ((bottom[0] - top[0]) * mix)),
                int(top[1] + ((bottom[1] - top[1]) * mix)),
                int(top[2] + ((bottom[2] - top[2]) * mix)),
            )
            pygame.draw.line(surface, color, (0, y), (self.layout.width, y))

    def _render_home_cards(self, surface: pygame.Surface, model: MenuRenderModel) -> None:
        start_x = 24
        start_y = 72
        gap = 14
        cols = 3
        card_w = (self.layout.width - (start_x * 2) - (gap * (cols - 1))) // cols
        card_h = 106
        for idx, row in enumerate(model.rows[:6]):
            col = idx % cols
            line = idx // cols
            x = start_x + (col * (card_w + gap))
            y = start_y + (line * (card_h + gap))
            card_rect = pygame.Rect(x, y, card_w, card_h)
            fill = (34, 56, 78) if row.selected else (22, 34, 50)
            border = (112, 206, 255) if row.selected else (52, 82, 110)
            pygame.draw.rect(surface, fill, card_rect, border_radius=14)
            pygame.draw.rect(surface, border, card_rect, width=2, border_radius=14)
            icon_center = (x + 24, y + 24)
            pygame.draw.circle(surface, border, icon_center, 10, width=2)
            label = self._small_font.render(row.label, True, (230, 236, 242))
            surface.blit(label, (x + 44, y + 14))
            hint = self._tiny_font.render("Open", True, (164, 182, 201))
            surface.blit(hint, (x + 44, y + 46))

    def _render_list(self, surface: pygame.Surface, model: MenuRenderModel) -> None:
        y = 72
        for row in model.rows:
            row_rect = pygame.Rect(20, y - 2, self.layout.width - 40, 28)
            if row.selected:
                pygame.draw.rect(surface, (38, 68, 102), row_rect, border_radius=8)
                pygame.draw.rect(surface, (108, 202, 255), row_rect, width=1, border_radius=8)
            label_text = self._small_font.render(row.label, True, (235, 240, 245))
            surface.blit(label_text, (28, y))
            if row.value_text:
                value_text = self._small_font.render(row.value_text, True, (171, 208, 232))
                value_rect = value_text.get_rect(midright=(self.layout.width - 28, y + 10))
                surface.blit(value_text, value_rect)
            y += 30
