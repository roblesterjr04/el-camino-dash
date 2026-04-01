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
from dashboard.screens.menu_overlay import _scroll_offset_px


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

        self._render_scroll_menu(surface, model)

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

    def _render_scroll_menu(self, surface: pygame.Surface, model: MenuRenderModel) -> None:
        body_top = 60
        footer_h = 52
        viewport_h = max(40, self.layout.height - body_top - footer_h)
        selected_idx = next((i for i, r in enumerate(model.rows) if r.selected), 0)
        row_gap = 4
        row_height = max(26, self._small_font.get_height() + 10)
        row_stride = row_height + row_gap
        scroll = _scroll_offset_px(
            selected_idx=selected_idx,
            num_rows=len(model.rows),
            row_height=row_height,
            row_gap=row_gap,
            viewport_h=viewport_h,
        )
        margin_x = 20
        viewport = pygame.Rect(margin_x, body_top, self.layout.width - 2 * margin_x, viewport_h)
        prev = surface.get_clip()
        surface.set_clip(viewport)
        try:
            for i, row in enumerate(model.rows):
                y = body_top - scroll + i * row_stride
                if y + row_height < body_top or y > body_top + viewport_h:
                    continue
                row_rect = pygame.Rect(margin_x, y, self.layout.width - 2 * margin_x, row_height)
                if row.selected:
                    pygame.draw.rect(surface, (38, 68, 102), row_rect, border_radius=8)
                    pygame.draw.rect(surface, (108, 202, 255), row_rect, width=1, border_radius=8)
                label_x = margin_x + 10
                label_text = self._small_font.render(row.label, True, (235, 240, 245))
                label_y = y + (row_height - label_text.get_height()) // 2
                value_margin_right = 10
                label_value_gap = 10
                if row.value_text:
                    value_text = self._small_font.render(row.value_text, True, (171, 208, 232))
                    vw = value_text.get_width()
                    right_x = self.layout.width - margin_x - value_margin_right
                    label_max_w = max(0, right_x - vw - label_value_gap - label_x)
                    label_clip = pygame.Rect(label_x, y, label_max_w, row_height)
                    clip_before = surface.get_clip()
                    surface.set_clip(label_clip.clip(clip_before))
                    surface.blit(label_text, (label_x, label_y))
                    surface.set_clip(clip_before)
                    value_rect = value_text.get_rect(midright=(right_x, y + row_height // 2))
                    surface.blit(value_text, value_rect)
                else:
                    surface.blit(label_text, (label_x, label_y))
        finally:
            surface.set_clip(prev)
