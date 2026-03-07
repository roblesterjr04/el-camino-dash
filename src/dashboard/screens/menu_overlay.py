from __future__ import annotations

import math
from pathlib import Path
from typing import Any, Callable

import pygame

from dashboard.screens.control_menu import ControlMenuController
from dashboard.screens.control_menu import MenuRenderModel
from dashboard.screens.control_menu import MenuSchemaError
from dashboard.screens.control_menu import load_menu_schema
from dashboard.screens.layout import DisplayLayout


class MenuOverlayScreen:
    def __init__(self, layout: DisplayLayout, config: dict[str, Any], close_enabled: bool) -> None:
        self.layout = layout
        self.config = config
        self._close_enabled = close_enabled
        self._compact = min(layout.width, layout.height) <= 260
        if self._compact:
            title_size = max(14, layout.height // 18)
            small_size = max(12, layout.height // 24)
            tiny_size = max(10, layout.height // 32)
        else:
            title_size = max(24, layout.height // 13)
            small_size = max(16, layout.height // 28)
            tiny_size = max(14, layout.height // 34)
        self._font = pygame.font.Font(config["themes"]["font_name"], title_size)
        self._small_font = pygame.font.Font(config["themes"]["font_name"], small_size)
        self._tiny_font = pygame.font.Font(config["themes"]["font_name"], tiny_size)
        self._audio_icon_base: pygame.Surface | None = None
        self._audio_icon_path = Path("assets/icons/audio_menu.png")

    def render(self, surface: pygame.Surface, model: MenuRenderModel) -> None:
        content_rect = self._paint_overlay(surface)
        header = self._font.render(model.title, True, (250, 246, 220))
        surface.blit(header, (content_rect.x, content_rect.y))
        line_gap = 4 if self._compact else 8
        line_y = content_rect.y + header.get_height() + line_gap
        pygame.draw.line(surface, (156, 122, 38), (content_rect.x, line_y), (content_rect.right, line_y), 2)
        body_top = line_y + (6 if self._compact else 10)
        if model.page_id == "home":
            self._render_home_carousel(surface, model, content_rect, body_top)
        else:
            self._render_list(surface, model, content_rect, body_top)
        close_hint = ""
        if self._close_enabled:
            close_hint = "x2:Close" if self._compact else "Double press: Close"
        footer_text = f"{model.footer}  {close_hint}".strip()
        footer = self._tiny_font.render(footer_text, True, (218, 196, 136))
        footer_y = content_rect.bottom - footer.get_height() - (4 if self._compact else 10)
        surface.blit(footer, (content_rect.x, footer_y))
        if model.message:
            message = self._tiny_font.render(model.message, True, (255, 224, 120))
            message_y = footer_y - message.get_height() - 4
            surface.blit(message, (content_rect.x, message_y))

    def render_error(self, surface: pygame.Surface, message: str) -> None:
        content_rect = self._paint_overlay(surface)
        header = self._font.render("Menu schema error", True, (255, 180, 160))
        surface.blit(header, (content_rect.x, content_rect.y))
        err = self._tiny_font.render(message[:120], True, (255, 120, 100))
        surface.blit(err, (content_rect.x, content_rect.y + header.get_height() + 8))

    def _paint_overlay(self, surface: pygame.Surface) -> pygame.Rect:
        shade = pygame.Surface(self.layout.size, pygame.SRCALPHA)
        if self.layout.shape == "round":
            center = (self.layout.width // 2, self.layout.height // 2)
            radius = max(1, (min(self.layout.width, self.layout.height) // 2) - 2)
            pygame.draw.circle(shade, (14, 10, 4, 195), center, radius)
            surface.blit(shade, (0, 0))
            panel_radius = max(1, radius - 12)
            pygame.draw.circle(surface, (40, 30, 12, 225), center, panel_radius)
            pygame.draw.circle(surface, (188, 148, 54, 235), center, panel_radius, width=2)
            # Use a conservative inscribed square so text stays inside circular bounds.
            safe_radius = max(1, int((panel_radius / math.sqrt(2)) * 0.86))
            return pygame.Rect(
                center[0] - safe_radius,
                center[1] - safe_radius,
                safe_radius * 2,
                safe_radius * 2,
            )
        shade.fill((14, 10, 4, 195))
        surface.blit(shade, (0, 0))
        panel = pygame.Rect(12, 12, self.layout.width - 24, self.layout.height - 24)
        pygame.draw.rect(surface, (40, 30, 12, 225), panel, border_radius=18)
        pygame.draw.rect(surface, (188, 148, 54, 235), panel, width=2, border_radius=18)
        return pygame.Rect(32, 24, self.layout.width - 64, self.layout.height - 60)

    def _render_home_carousel(
        self, surface: pygame.Surface, model: MenuRenderModel, content_rect: pygame.Rect, body_top: int
    ) -> None:
        if not model.rows:
            return
        selected_idx = 0
        for idx, row in enumerate(model.rows):
            if row.selected:
                selected_idx = idx
                break
        center_x = content_rect.centerx
        footer_reserve = self._tiny_font.get_height() + (20 if self._compact else 34)
        body_bottom = max(body_top + 24, content_rect.bottom - footer_reserve)
        center_y = body_top + ((body_bottom - body_top) // 2)
        slot_width = max(46 if self._compact else 78, content_rect.width // 4)
        for idx, row in enumerate(model.rows):
            distance = idx - selected_idx
            if abs(distance) > 2:
                continue
            x = center_x + (distance * slot_width)
            if x < content_rect.left or x > content_rect.right:
                continue
            radius = max(16 if self._compact else 24, (36 if self._compact else 52) - (abs(distance) * 8))
            selected = distance == 0
            fill = (130, 96, 24) if selected else (70, 50, 14)
            border = (255, 215, 96) if selected else (186, 146, 58)
            pygame.draw.circle(surface, fill, (x, center_y), radius)
            pygame.draw.circle(surface, border, (x, center_y), radius, width=3 if selected else 2)
            icon_color = (250, 246, 220) if selected else (236, 216, 162)
            self._draw_menu_icon(
                surface=surface,
                center=(x, center_y),
                radius=max(14, radius - 10),
                icon_id=str(row.icon or row.item_id or row.label).strip().lower(),
                color=icon_color,
            )
            label = self._small_font.render(row.label, True, (246, 232, 176) if selected else (206, 184, 120))
            label_offset = 14 if self._compact else 24
            label_rect = label.get_rect(center=(x, center_y + radius + label_offset))
            surface.blit(label, label_rect)

    def _draw_menu_icon(
        self,
        surface: pygame.Surface,
        center: tuple[int, int],
        radius: int,
        icon_id: str,
        color: tuple[int, int, int],
    ) -> None:
        cx, cy = center
        icon = icon_id.lower()
        width = max(2, radius // 8)
        if icon in {"clock", "now", "now_page"}:
            pygame.draw.circle(surface, color, (cx, cy), radius, width=width)
            pygame.draw.line(surface, color, (cx, cy), (cx, cy - int(radius * 0.5)), width=max(2, width - 1))
            pygame.draw.line(surface, color, (cx, cy), (cx + int(radius * 0.45), cy), width=max(2, width - 1))
            return
        if icon in {"media", "music", "med", "media_page"}:
            stem_h = int(radius * 1.1)
            stem_x = cx + int(radius * 0.15)
            stem_top = cy - int(radius * 0.9)
            stem_bottom = stem_top + stem_h
            pygame.draw.line(surface, color, (stem_x, stem_top), (stem_x, stem_bottom), width=width)
            pygame.draw.line(
                surface,
                color,
                (stem_x, stem_top),
                (stem_x + int(radius * 0.6), stem_top + int(radius * 0.2)),
                width=width,
            )
            pygame.draw.circle(surface, color, (cx - int(radius * 0.35), cy + int(radius * 0.45)), int(radius * 0.28), width=0)
            pygame.draw.circle(surface, color, (cx + int(radius * 0.25), cy + int(radius * 0.55)), int(radius * 0.28), width=0)
            return
        if icon in {"audio", "audio_page"}:
            self._draw_audio_icon(surface=surface, center=center, radius=radius, color=color)
            return
        if icon in {"inputs", "in", "inputs_page"}:
            for idx, y_off in enumerate((-int(radius * 0.45), 0, int(radius * 0.45))):
                y = cy + y_off
                pygame.draw.line(surface, color, (cx - int(radius * 0.9), y), (cx + int(radius * 0.9), y), width=max(2, width - 1))
                knob_x = cx + (int(radius * 0.3) if idx % 2 == 0 else -int(radius * 0.3))
                pygame.draw.circle(surface, color, (knob_x, y), max(3, radius // 4), width=0)
            return
        if icon in {"system", "sys", "settings", "gear", "system_page"}:
            core_r = max(4, int(radius * 0.28))
            gear_r = max(core_r + 2, int(radius * 0.68))
            pygame.draw.circle(surface, color, (cx, cy), gear_r, width=width)
            pygame.draw.circle(surface, color, (cx, cy), core_r, width=width)
            for i in range(8):
                angle = (math.pi * 2.0 * i) / 8.0
                inner = (cx + int(math.cos(angle) * (gear_r - width)), cy + int(math.sin(angle) * (gear_r - width)))
                outer = (cx + int(math.cos(angle) * (gear_r + int(radius * 0.18))), cy + int(math.sin(angle) * (gear_r + int(radius * 0.18))))
                pygame.draw.line(surface, color, inner, outer, width=max(2, width - 1))
            return
        if icon in {"alert", "alerts", "warning", "fault", "alerts_page"}:
            tri = [
                (cx, cy - int(radius * 0.9)),
                (cx - int(radius * 0.9), cy + int(radius * 0.8)),
                (cx + int(radius * 0.9), cy + int(radius * 0.8)),
            ]
            pygame.draw.polygon(surface, color, tri, width=max(2, width))
            pygame.draw.line(
                surface,
                color,
                (cx, cy - int(radius * 0.25)),
                (cx, cy + int(radius * 0.3)),
                width=max(2, width - 1),
            )
            pygame.draw.circle(surface, color, (cx, cy + int(radius * 0.55)), max(2, radius // 8), width=0)
            return
        token = (icon_id or "?").strip().upper()[:3] or "?"
        glyph = self._small_font.render(token, True, color)
        surface.blit(glyph, glyph.get_rect(center=(cx, cy)))

    def _draw_audio_icon(
        self,
        surface: pygame.Surface,
        center: tuple[int, int],
        radius: int,
        color: tuple[int, int, int],
    ) -> None:
        icon = self._load_audio_icon_surface()
        if icon is None:
            # Fallback to vector icon if asset is missing.
            cx, cy = center
            width = max(2, radius // 8)
            body_w = int(radius * 0.75)
            body_h = int(radius * 0.72)
            body = pygame.Rect(cx - int(radius * 0.9), cy - (body_h // 2), body_w, body_h)
            pygame.draw.rect(surface, color, body, width=0, border_radius=max(2, width))
            tip = [
                (body.right - int(radius * 0.08), cy - int(radius * 0.55)),
                (cx + int(radius * 0.5), cy - int(radius * 0.85)),
                (cx + int(radius * 0.5), cy + int(radius * 0.85)),
                (body.right - int(radius * 0.08), cy + int(radius * 0.55)),
            ]
            pygame.draw.polygon(surface, color, tip)
            for scale in (0.42, 0.68):
                pygame.draw.arc(
                    surface,
                    color,
                    pygame.Rect(
                        cx + int(radius * 0.25),
                        cy - int(radius * scale),
                        int(radius * scale * 1.2),
                        int(radius * scale * 2.0),
                    ),
                    -0.95,
                    0.95,
                    width=max(2, width - 1),
                )
            return

        target = max(10, int(radius * 2.2))
        scaled = pygame.transform.smoothscale(icon, (target, target))
        icon_mask = pygame.mask.from_surface(scaled)
        tinted = icon_mask.to_surface(setcolor=(*color, 255), unsetcolor=(0, 0, 0, 0))
        surface.blit(tinted, tinted.get_rect(center=center))

    def _load_audio_icon_surface(self) -> pygame.Surface | None:
        if self._audio_icon_base is not None:
            return self._audio_icon_base
        if not self._audio_icon_path.exists():
            return None
        try:
            self._audio_icon_base = pygame.image.load(str(self._audio_icon_path)).convert_alpha()
        except pygame.error:
            self._audio_icon_base = None
        return self._audio_icon_base

    def _render_list(self, surface: pygame.Surface, model: MenuRenderModel, content_rect: pygame.Rect, body_top: int) -> None:
        y = body_top
        row_height = max(self._small_font.get_height() + (8 if self._compact else 12), 22)
        selected_idx = 0
        for idx, row in enumerate(model.rows):
            if row.selected:
                selected_idx = idx
                break
        # Keep the selected entry pinned at the top for small displays.
        ordered_rows = model.rows[selected_idx:] + model.rows[:selected_idx]
        footer_reserve = self._tiny_font.get_height() + (20 if self._compact else 34)
        row_gap = 2 if self._compact else 4
        for row in ordered_rows:
            if y > content_rect.bottom - footer_reserve:
                break
            row_rect = pygame.Rect(content_rect.x, y - 4, content_rect.width, row_height)
            if row.selected:
                pygame.draw.rect(surface, (96, 70, 18), row_rect, border_radius=9)
                pygame.draw.rect(surface, (252, 208, 88), row_rect, width=1, border_radius=9)
            label_text = self._small_font.render(row.label, True, (248, 240, 206))
            label_y = row_rect.centery - (label_text.get_height() // 2)
            surface.blit(label_text, (content_rect.x + 8, label_y))
            if row.value_text:
                value_text = self._small_font.render(row.value_text, True, (240, 210, 128))
                value_rect = value_text.get_rect(midright=(content_rect.right - 8, row_rect.centery))
                surface.blit(value_text, value_rect)
            y += row_height + row_gap


class MenuOverlayController:
    def __init__(
        self,
        layout: DisplayLayout,
        config: dict[str, Any],
        config_path: Path,
        command_registry: dict[str, Callable[[dict[str, Any]], str | None]],
        on_runtime_refresh: Callable[[], None] | None = None,
    ) -> None:
        system_cfg = config.get("system", {})
        keyboard_sim_cfg = config.get("inputs", {}).get("keyboard_sim", {})
        keyboard_sim_active = bool(keyboard_sim_cfg.get("enabled", False))
        simulation_mode = str(system_cfg.get("mode", "")).strip().lower() == "simulation"
        default_close_ms = 0 if (simulation_mode and keyboard_sim_active) else 450
        self._visible = False
        self._input_initialized = False
        self._last_press_count = 0
        self._last_diag_open_count = 0
        self._last_press_tick_ms = -1000
        self._close_double_press_ms = max(0, int(system_cfg.get("menu_close_double_press_ms", default_close_ms)))
        self._menu_error = ""
        self._menu: ControlMenuController | None = None
        self._screen = MenuOverlayScreen(
            layout=layout,
            config=config,
            close_enabled=self._close_double_press_ms > 0,
        )
        schema_path = config["system"].get("menu_schema_path", "config/menu_schema.yaml")
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

    @property
    def visible(self) -> bool:
        return self._visible

    def process_input(self, values: dict[str, float], status: dict[str, str]) -> None:
        delta = int(values.get("inputs.rotary.menu.delta", 0.0))
        press_count = int(values.get("inputs.rotary.menu.presses", 0.0))
        diag_open_count = int(values.get("inputs.ir.command.diagnostics_open_count", 0.0))
        if not self._input_initialized:
            # Hot-reload/re-render can recreate this controller while input counters
            # stay cumulative; baseline them so overlay never auto-opens.
            self._last_press_count = press_count
            self._last_diag_open_count = diag_open_count
            self._input_initialized = True
            return
        if diag_open_count < self._last_diag_open_count:
            self._last_diag_open_count = diag_open_count
        if diag_open_count > self._last_diag_open_count:
            self._visible = True
            if self._menu:
                if not self._menu.open_page("ir_diagnostics"):
                    self._menu.open_page("system")
                self._menu.set_message("IR diagnostics active")
            self._last_diag_open_count = diag_open_count
        if press_count < self._last_press_count:
            self._last_press_count = press_count
        new_presses = max(0, press_count - self._last_press_count)
        if new_presses > 0:
            # Treat select as edge-triggered once per frame. This prevents a
            # single key press from being interpreted as multiple activations.
            new_presses = 1
        # Prioritize press events over rotate in the same frame so a lingering
        # rotate input (e.g. keyboard key release timing) does not shift the
        # highlighted item just before activation.
        if self._visible and self._menu and delta != 0 and new_presses == 0:
            self._menu.rotate(delta)
        if new_presses > 0 and not self._visible:
            # Opening the overlay should never cascade into immediate selection.
            self._visible = True
            self._last_press_tick_ms = pygame.time.get_ticks()
        elif new_presses > 0:
            now_ms = pygame.time.get_ticks()
            if self._close_double_press_ms > 0 and now_ms - self._last_press_tick_ms <= self._close_double_press_ms:
                self._hide_overlay()
            elif self._menu:
                self._menu.press(status)
            self._last_press_tick_ms = now_ms
        self._last_press_count = press_count

    def _hide_overlay(self) -> None:
        self._visible = False
        if self._menu:
            self._menu.reset_view()

    def render(self, surface: pygame.Surface, status: dict[str, str]) -> None:
        if not self._visible:
            return
        if not self._menu:
            self._screen.render_error(surface=surface, message=self._menu_error)
            return
        model = self._menu.render_model(status=status)
        self._screen.render(surface=surface, model=model)
