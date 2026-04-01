from __future__ import annotations

import math
from pathlib import Path
from typing import Any, Callable

import pygame

from dashboard.screens.control_menu import ControlMenuController
from dashboard.screens.control_menu import MenuRenderModel
from dashboard.screens.control_menu import MenuRenderRow
from dashboard.screens.control_menu import MenuSchemaError
from dashboard.screens.control_menu import load_menu_schema
from dashboard.screens.layout import DisplayLayout

# Matches encoder OLED buffers: title 31 chars; M0–M2 / V0–V2 tokens trim to 31.
_OLED_TITLE_MAX = 31
_OLED_TOKEN_MAX = 31
# Physical OLED shows three stacked rows + scrollbar (encoder_controller drawMenuScroll).
_OLED_MENU_VISIBLE_ROWS = 3

# Menu row: label left, status/value right — reserve space so values stay right-justified.
_MENU_VALUE_MARGIN_RIGHT = 8
_MENU_LABEL_VALUE_GAP = 10


def _oled_menu_label_value(row: MenuRenderRow) -> tuple[str, str]:
    """Label and value for Nano UART (M0–M2 + V0–V2); firmware draws label left, value right."""
    lab = row.label.strip()[:_OLED_TOKEN_MAX]
    val = (row.value_text or "").strip()[:_OLED_TOKEN_MAX]
    return lab, val


def _oled_first_visible_row(total: int, selected_idx: int, visible: int) -> int:
    if total <= 0 or visible <= 0:
        return 0
    if total <= visible:
        return 0
    ideal = selected_idx - visible // 2
    return max(0, min(ideal, total - visible))


def _menu_model_to_oled_scroll(model: MenuRenderModel) -> dict[str, str | int]:
    """Payload for Nano 256×64 vertical list (M0–M2, MT, MO, MG)."""
    title = model.title.replace("  * unsaved", "*").strip()[:_OLED_TITLE_MAX]
    rows = model.rows
    n = len(rows)
    if n == 0:
        return {
            "title": title or "-",
            "m0": "",
            "m1": "",
            "m2": "",
            "v0": "",
            "v1": "",
            "v2": "",
            "total": 0,
            "offset": 0,
            "sel": 0,
        }
    sel = next((i for i, r in enumerate(rows) if r.selected), 0)
    mo = _oled_first_visible_row(n, sel, _OLED_MENU_VISIBLE_ROWS)
    labs: list[str] = []
    vals: list[str] = []
    for i in range(_OLED_MENU_VISIBLE_ROWS):
        idx = mo + i
        if idx < n:
            lab, val = _oled_menu_label_value(rows[idx])
            labs.append(lab)
            vals.append(val)
        else:
            labs.append("")
            vals.append("")
    return {
        "title": title or "-",
        "m0": labs[0],
        "m1": labs[1],
        "m2": labs[2],
        "v0": vals[0],
        "v1": vals[1],
        "v2": vals[2],
        "total": n,
        "offset": mo,
        "sel": sel,
    }


def _scroll_offset_px(
    selected_idx: int,
    num_rows: int,
    row_height: int,
    row_gap: int,
    viewport_h: int,
) -> int:
    """Pixel offset so the list scrolls vertically; keeps the selection near the viewport center."""
    if num_rows <= 0 or viewport_h <= 0:
        return 0
    row_stride = row_height + row_gap
    total_h = num_rows * row_stride - row_gap
    if total_h <= viewport_h:
        return 0
    sel_center = selected_idx * row_stride + row_height // 2
    ideal = sel_center - viewport_h // 2
    max_scroll = max(0, total_h - viewport_h)
    return max(0, min(ideal, max_scroll))


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
        self._render_scroll_menu(surface, model, content_rect, body_top)
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

    def _render_scroll_menu(
        self, surface: pygame.Surface, model: MenuRenderModel, content_rect: pygame.Rect, body_top: int
    ) -> None:
        """Traditional vertical list: fixed row order, scroll to keep selection in view."""
        if not model.rows:
            return
        selected_idx = 0
        for idx, row in enumerate(model.rows):
            if row.selected:
                selected_idx = idx
                break
        footer_reserve = self._tiny_font.get_height() + (20 if self._compact else 34)
        viewport_bottom = content_rect.bottom - footer_reserve
        viewport_h = max(0, viewport_bottom - body_top)
        row_gap = 2 if self._compact else 4
        row_height = max(self._small_font.get_height() + (10 if self._compact else 14), 30 if self._compact else 40)
        row_stride = row_height + row_gap
        icon_col = 40 if self._compact else 52
        icon_r = 12 if self._compact else 16

        scroll = _scroll_offset_px(
            selected_idx=selected_idx,
            num_rows=len(model.rows),
            row_height=row_height,
            row_gap=row_gap,
            viewport_h=viewport_h,
        )
        total_h = len(model.rows) * row_stride - row_gap
        max_scroll = max(0, total_h - viewport_h)

        viewport = pygame.Rect(content_rect.x, body_top, content_rect.width, viewport_h)
        prev_clip = surface.get_clip()
        surface.set_clip(viewport)

        try:
            for i, row in enumerate(model.rows):
                y = body_top - scroll + i * row_stride
                if y > viewport_bottom or y + row_height < body_top:
                    continue
                row_rect = pygame.Rect(content_rect.x, y, content_rect.width, row_height)
                if row.selected:
                    pygame.draw.rect(surface, (96, 70, 18), row_rect, border_radius=9)
                    pygame.draw.rect(surface, (252, 208, 88), row_rect, width=1, border_radius=9)
                cx = content_rect.x + icon_col // 2
                cy = y + row_height // 2
                icon_color = (250, 246, 220) if row.selected else (236, 216, 162)
                pygame.draw.circle(surface, (55, 42, 14) if row.selected else (45, 34, 10), (cx, cy), icon_r + 2)
                self._draw_menu_icon(
                    surface=surface,
                    center=(cx, cy),
                    radius=icon_r,
                    icon_id=str(row.icon or row.item_id or row.label).strip().lower(),
                    color=icon_color,
                )
                label_x = content_rect.x + icon_col + 6
                label_text = self._small_font.render(row.label, True, (248, 240, 206))
                label_y = y + (row_height - label_text.get_height()) // 2
                if row.value_text:
                    value_text = self._small_font.render(row.value_text, True, (240, 210, 128))
                    vw = value_text.get_width()
                    label_max_w = max(
                        0,
                        content_rect.right
                        - _MENU_VALUE_MARGIN_RIGHT
                        - vw
                        - _MENU_LABEL_VALUE_GAP
                        - label_x,
                    )
                    label_clip = pygame.Rect(label_x, y, label_max_w, row_height)
                    clip_before = surface.get_clip()
                    surface.set_clip(label_clip.clip(clip_before))
                    surface.blit(label_text, (label_x, label_y))
                    surface.set_clip(clip_before)
                    value_rect = value_text.get_rect(
                        midright=(content_rect.right - _MENU_VALUE_MARGIN_RIGHT, y + row_height // 2)
                    )
                    surface.blit(value_text, value_rect)
                else:
                    surface.blit(label_text, (label_x, label_y))
        finally:
            surface.set_clip(prev_clip)

        if max_scroll > 0:
            hint_c = (200, 170, 90)
            if scroll > 2:
                up = self._tiny_font.render("\u25b2", True, hint_c)
                surface.blit(up, (content_rect.right - 22, body_top))
            if scroll < max_scroll - 2:
                down = self._tiny_font.render("\u25bc", True, hint_c)
                surface.blit(down, (content_rect.right - 22, viewport_bottom - self._tiny_font.get_height()))

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
        if icon in {"wifi", "wifi_page", "wireless"}:
            # Curved arcs (signal fan) with a dot
            dot_r = max(3, radius // 5)
            pygame.draw.circle(surface, color, (cx, cy + int(radius * 0.55)), dot_r, width=0)
            for span in (0.95, 0.72, 0.48):
                r = int(radius * span)
                rect = pygame.Rect(cx - r, cy - r + int(radius * 0.15), r * 2, r * 2)
                pygame.draw.arc(surface, color, rect, math.pi * 1.15, math.pi * 1.85, width=max(2, width))
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
        if icon in {"close", "close_menu", "exit", "quit", "exit_app"}:
            # Doorway + arrow out (read left-to-right as "leave")
            w = int(radius * 0.85)
            h = int(radius * 1.1)
            rect = pygame.Rect(cx - int(radius * 0.55), cy - h // 2, w, h)
            pygame.draw.rect(surface, color, rect, width=max(2, width))
            ax0 = cx + int(radius * 0.2)
            pygame.draw.line(
                surface,
                color,
                (ax0, cy),
                (cx + int(radius * 0.95), cy),
                width=max(2, width + 1),
            )
            pygame.draw.line(
                surface,
                color,
                (cx + int(radius * 0.65), cy - int(radius * 0.35)),
                (cx + int(radius * 0.95), cy),
                width=max(2, width + 1),
            )
            pygame.draw.line(
                surface,
                color,
                (cx + int(radius * 0.65), cy + int(radius * 0.35)),
                (cx + int(radius * 0.95), cy),
                width=max(2, width + 1),
            )
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
        if not self._input_initialized:
            # Hot-reload/re-render can recreate this controller while input counters
            # stay cumulative; baseline them so overlay never auto-opens.
            self._last_press_count = press_count
            self._input_initialized = True
            return
        if press_count < self._last_press_count:
            self._last_press_count = press_count
        new_presses = max(0, press_count - self._last_press_count)
        if new_presses > 0:
            now_ms = pygame.time.get_ticks()
            # Two physical presses can arrive in one poll (serial burst). Double-tap-to-close
            # needs both edges; the old "cap to 1" dropped the second.
            if (
                self._visible
                and self._close_double_press_ms > 0
                and new_presses >= 2
                and now_ms - self._last_press_tick_ms <= self._close_double_press_ms
            ):
                self._hide_overlay()
                self._last_press_count = press_count
                self._last_press_tick_ms = now_ms
                return
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

    def close_overlay(self) -> None:
        self._hide_overlay()

    def open_or_back(self) -> str:
        if not self._visible:
            self._visible = True
            return "Menu opened"
        if self._menu and self._menu.back_one():
            return "Menu back"
        self._hide_overlay()
        return "Menu closed"

    def _hide_overlay(self) -> None:
        self._visible = False
        if self._menu:
            self._menu.reset_view()

    def oled_menu_nano_payload(self, status: dict[str, str]) -> dict[str, str | int] | None:
        """Structured menu for Nano UART (vertical scroll list); None if menu is closed."""
        if not self._visible or not self._menu:
            return None
        model = self._menu.render_model(status=status)
        return _menu_model_to_oled_scroll(model)

    def render(self, surface: pygame.Surface, status: dict[str, str]) -> None:
        if not self._visible:
            return
        if not self._menu:
            self._screen.render_error(surface=surface, message=self._menu_error)
            return
        model = self._menu.render_model(status=status)
        self._screen.render(surface=surface, model=model)
