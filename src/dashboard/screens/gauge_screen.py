from __future__ import annotations

from datetime import datetime
import json
import math
from pathlib import Path
from typing import Any

import pygame

from dashboard.screens.layout import DisplayLayout


class GaugeScreen:
    def __init__(self, layout: DisplayLayout, config: dict[str, Any]) -> None:
        self.layout = layout
        self.config = config
        self._font = pygame.font.Font(config["themes"]["font_name"], max(18, layout.height // 16))
        self._font_cache: dict[str, pygame.font.Font] = {}
        self._theme_data = self._load_theme_data()
        self._sprite_cache = self._load_sprites()
        self._theme_sprite_cache: dict[str, pygame.Surface] = {}

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

    def _load_sprites(self) -> dict[str, pygame.Surface]:
        sprite_key = self._active_sprite_key()
        if not sprite_key:
            return {}
        sprite_theme_root = Path(self.config["themes"]["gauge_asset_root"]) / self.config["themes"]["active"]
        sprite_pack = self._theme_data.get("sprite_packs", {}).get(sprite_key, sprite_key)
        sprite_root = sprite_theme_root / str(sprite_pack)
        sprite_files = self._theme_data.get("sprite_files", {})
        sprite_overrides = self._theme_data.get("sprite_overrides", {}).get(sprite_key, {})
        sprites: dict[str, pygame.Surface] = {}
        for name in ("dial", "needle", "overlay"):
            override_filename = sprite_overrides.get(name)
            if override_filename:
                flat_path = sprite_theme_root / str(override_filename)
                if flat_path.exists():
                    surface = self._try_load_sprite_surface(flat_path)
                    if surface is not None:
                        sprites[name] = surface
                        continue

            sprite_filename = str(sprite_files.get(name, f"{name}.png"))
            packed_path = sprite_root / sprite_filename
            if packed_path.exists():
                surface = self._try_load_sprite_surface(packed_path)
                if surface is not None:
                    sprites[name] = surface
                    continue

            flat_default = sprite_theme_root / sprite_filename
            if flat_default.exists():
                surface = self._try_load_sprite_surface(flat_default)
                if surface is not None:
                    sprites[name] = surface
        return sprites

    def _active_sprite_key(self) -> str:
        if self._is_center_clock_mode():
            return "clock"
        return self.layout.gauge_id or ""

    def _load_sprite_surface(self, sprite_path: Path) -> pygame.Surface:
        loaded = pygame.image.load(str(sprite_path))
        # Prefer alpha conversion when a display format is available.
        try:
            return loaded.convert_alpha()
        except pygame.error:
            pass
        return loaded

    def _try_load_sprite_surface(self, sprite_path: Path) -> pygame.Surface | None:
        try:
            return self._load_sprite_surface(sprite_path)
        except (FileNotFoundError, OSError, pygame.error):
            # Gracefully fall back to procedural rendering when sprite files
            # are missing or unreadable in the current environment.
            return None

    def _gauge_spec(self) -> dict[str, Any]:
        if not self.layout.gauge_id:
            return {}
        return self.config["gauges"][self.layout.gauge_id]

    def render(self, surface: pygame.Surface, values: dict[str, float], status: dict[str, str]) -> None:
        spec = self._gauge_spec()
        if not spec:
            return
        surface.fill(self._screen_background_color())

        source = spec["source"]
        smoothed_key = f"gauges.{self.layout.gauge_id}.value"
        raw_value = float(values.get(smoothed_key, values.get(source, 0.0)))
        source_value, unit_override = self._apply_source_calibration(source, raw_value)
        visuals = self._visual_settings()
        value = self._apply_primary_calibration(source_value, visuals)
        min_value = float(visuals.get("visual_min", spec["min_value"]))
        max_value = float(visuals.get("visual_max", spec["max_value"]))
        ratio = self._compute_primary_ratio(
            value=value,
            spec=spec,
            visuals=visuals,
            source_unit_override=unit_override,
            visual_min=min_value,
            visual_max=max_value,
        )
        startup_ratio = values.get("system.startup_sweep_ratio")
        if startup_ratio is not None:
            try:
                ratio = max(0.0, min(1.0, float(startup_ratio)))
            except (TypeError, ValueError):
                pass

        cx = self.layout.width // 2
        cy = self.layout.height // 2
        radius = min(cx, cy) - 8

        mode_key = "clock" if self._is_center_clock_mode() else (self.layout.gauge_id or "")
        if self._gauge_mode_for_key(mode_key) == "digital":
            if mode_key == "clock":
                self._render_digital_clock(surface, status=status)
            else:
                self._render_digital_gauge(
                    surface=surface,
                    spec=spec,
                    value=value,
                    ratio=ratio,
                    values=values,
                    status=status,
                    display_unit=unit_override or str(spec.get("unit", "")),
                    visual_min=min_value,
                    visual_max=max_value,
                    redline_cfg=visuals.get("redline", {}),
                    invert_value=bool(visuals.get("invert_value", False)),
                )
            return

        if self._is_center_clock_mode():
            self._render_center_clock(surface, values, status, spec, cx, cy, radius)
            return

        draw_under_dial = self._theme_flag_for_gauge("needle_under_dial", False)
        gauge_key = self.layout.gauge_id or ""
        show_ticks = self._theme_flag_for_specific_key("show_ticks", gauge_key, True)
        debug_ticks_on_top = self._theme_flag_for_gauge("debug_ticks_on_top", False)
        hide_dial_sprite = self._theme_flag_for_gauge("debug_hide_dial_sprite", False)
        has_visible_dial_sprite = self._sprite_cache.get("dial") is not None and not hide_dial_sprite
        if show_ticks and not debug_ticks_on_top and has_visible_dial_sprite:
            # Ticks are always the lowest z-layer.
            self._draw_ticks(surface, cx, cy, radius, spec=spec, visuals=visuals)
        if draw_under_dial:
            if self._theme_flag_for_gauge("show_primary_needle", True):
                self._draw_needle(surface, cx, cy, radius, ratio)
            self._draw_sub_gauges(surface, values)
            self._draw_background(surface, cx, cy, radius)
            self._draw_overlay(surface, cx, cy, radius)
        else:
            self._draw_background(surface, cx, cy, radius)
            if self._theme_flag_for_gauge("show_primary_needle", True):
                self._draw_needle(surface, cx, cy, radius, ratio)
            self._draw_sub_gauges(surface, values)
            self._draw_overlay(surface, cx, cy, radius)
        if show_ticks and not debug_ticks_on_top and not has_visible_dial_sprite:
            # In draw-only mode (no dial sprite), draw ticks after fallback background
            # so they are visible for debugging and baseline themes.
            self._draw_ticks(surface, cx, cy, radius, spec=spec, visuals=visuals)
        self._draw_indicator_elements(surface, values)
        if show_ticks and debug_ticks_on_top:
            # Debug mode: force ticks above dial/sprites for visibility checks.
            self._draw_ticks(surface, cx, cy, radius, spec=spec, visuals=visuals)
        if self._theme_flag_for_gauge("show_text", True):
            self._draw_text(surface, spec, value, values, status, unit_override)
        if self._theme_flag("show_odometer", True):
            self._draw_odometer(surface, spec, values)

    def _is_center_clock_mode(self) -> bool:
        if self.layout.display_id != "gauge_center":
            return False
        mode_cfg = self._theme_data.get("center_dial_mode", "tach")
        mode = "tach"
        if isinstance(mode_cfg, str):
            mode = mode_cfg
        elif isinstance(mode_cfg, dict):
            display_mode = mode_cfg.get(self.layout.display_id)
            if isinstance(display_mode, str):
                mode = display_mode
            elif isinstance(mode_cfg.get("_default"), str):
                mode = str(mode_cfg["_default"])
        return mode.strip().lower() == "clock"

    def _render_center_clock(
        self,
        surface: pygame.Surface,
        values: dict[str, float],
        status: dict[str, str],
        spec: dict[str, Any],
        cx: int,
        cy: int,
        radius: int,
    ) -> None:
        clock_visuals = self._clock_visual_settings()
        gauge_key = self.layout.gauge_id or ""
        draw_under_dial = self._theme_flag_for_specific_key(
            "needle_under_dial",
            "clock",
            self._theme_flag_for_gauge("needle_under_dial", False),
        )
        show_clock_hands = self._theme_flag_for_specific_key(
            "show_primary_needle",
            "clock",
            self._theme_flag_for_gauge("show_primary_needle", True),
        )
        show_ticks = self._theme_flag_for_specific_key(
            "show_ticks",
            "clock",
            self._theme_flag_for_specific_key("show_ticks", gauge_key, True),
        )
        debug_ticks_on_top = self._theme_flag_for_gauge("debug_ticks_on_top", False)
        hide_dial_sprite = self._theme_flag_for_gauge("debug_hide_dial_sprite", False)
        has_visible_dial_sprite = self._sprite_cache.get("dial") is not None and not hide_dial_sprite
        if show_ticks and not debug_ticks_on_top and has_visible_dial_sprite:
            self._draw_ticks(surface, cx, cy, radius, spec=spec, visuals=clock_visuals, tick_key="clock")
        if draw_under_dial:
            if show_clock_hands:
                self._draw_clock_hands(surface, cx, cy)
            self._draw_background(surface, cx, cy, radius)
            self._draw_overlay(surface, cx, cy, radius)
        else:
            self._draw_background(surface, cx, cy, radius)
            if show_clock_hands:
                self._draw_clock_hands(surface, cx, cy)
            self._draw_overlay(surface, cx, cy, radius)
        if show_ticks and not debug_ticks_on_top and not has_visible_dial_sprite:
            self._draw_ticks(surface, cx, cy, radius, spec=spec, visuals=clock_visuals, tick_key="clock")
        self._draw_indicator_elements(surface, values)
        if show_ticks and debug_ticks_on_top:
            self._draw_ticks(surface, cx, cy, radius, spec=spec, visuals=clock_visuals, tick_key="clock")
        if self._theme_flag_for_gauge("show_text", True):
            now = datetime.now()
            clock_label = f"{now:%I:%M}"
            text = self._font.render(clock_label, True, (240, 240, 240))
            text_rect = text.get_rect(center=(self.layout.width // 2, int(self.layout.height * 0.78)))
            surface.blit(text, text_rect)
        if self._theme_flag("show_odometer", True):
            self._draw_odometer(surface, spec, values)
        if status.get("theme_warning"):
            warning_font = pygame.font.Font(self.config["themes"]["font_name"], max(14, self.layout.height // 24))
            warning = warning_font.render(status["theme_warning"], True, (255, 200, 0))
            warning_rect = warning.get_rect(center=(self.layout.width // 2, int(self.layout.height * 0.9)))
            surface.blit(warning, warning_rect)

    def _clock_render_config(self) -> dict[str, Any]:
        cfg = self._theme_data.get("clock_render", {})
        if not isinstance(cfg, dict):
            return {}
        merged: dict[str, Any] = {}
        base = cfg.get("_default", {})
        if isinstance(base, dict):
            merged.update(base)
        display_cfg = cfg.get(self.layout.display_id, {})
        if isinstance(display_cfg, dict):
            merged.update(display_cfg)
        gauge_id = self.layout.gauge_id or ""
        gauge_cfg = cfg.get(gauge_id, {})
        if isinstance(gauge_cfg, dict):
            merged.update(gauge_cfg)
        return merged

    def _clock_visual_settings(self) -> dict[str, Any]:
        visuals = self._visual_settings_for_keys(["clock", self.layout.display_id])
        if visuals:
            return visuals
        return self._visual_settings()

    def _draw_clock_hands(self, surface: pygame.Surface, center_x: int, center_y: int) -> None:
        now = datetime.now()
        minute_ratio = (now.minute + (now.second / 60.0)) / 60.0
        hour_ratio = ((now.hour % 12) + minute_ratio) / 12.0
        minute_user_angle = (180.0 + (minute_ratio * 360.0)) % 360.0
        hour_user_angle = (180.0 + (hour_ratio * 360.0)) % 360.0
        cfg = self._clock_render_config()
        needle = self._sprite_cache.get("needle")
        if needle:
            self._draw_clock_sprite_hand(
                surface=surface,
                needle=needle,
                center_x=center_x,
                center_y=center_y,
                user_angle_deg=hour_user_angle,
                scale=float(cfg.get("hour_scale", 0.20)),
                pivot=cfg.get("pivot", [0.5, 0.94]),
                angle_offset=float(cfg.get("angle_offset_deg", -90.0)),
            )
            self._draw_clock_sprite_hand(
                surface=surface,
                needle=needle,
                center_x=center_x,
                center_y=center_y,
                user_angle_deg=minute_user_angle,
                scale=float(cfg.get("minute_scale", 0.30)),
                pivot=cfg.get("pivot", [0.5, 0.94]),
                angle_offset=float(cfg.get("angle_offset_deg", -90.0)),
            )
            return
        color = tuple(self._theme_data.get("fallback_palette", {}).get("needle", [235, 70, 65]))
        min_dim = min(self.layout.width, self.layout.height)
        hour_len = int(min_dim * 0.20)
        minute_len = int(min_dim * 0.30)
        self._draw_clock_line_hand(surface, center_x, center_y, hour_user_angle, hour_len, color, 4)
        self._draw_clock_line_hand(surface, center_x, center_y, minute_user_angle, minute_len, color, 3)

    def _draw_clock_sprite_hand(
        self,
        surface: pygame.Surface,
        needle: pygame.Surface,
        center_x: int,
        center_y: int,
        user_angle_deg: float,
        scale: float,
        pivot: Any,
        angle_offset: float,
    ) -> None:
        scale = max(0.02, float(scale))
        target = max(6, int(min(self.layout.width, self.layout.height) * scale))
        w, h = needle.get_size()
        if w <= 0 or h <= 0:
            return
        if w >= h:
            new_w = target
            new_h = max(1, int((h / w) * target))
        else:
            new_h = target
            new_w = max(1, int((w / h) * target))
        resized = pygame.transform.smoothscale(needle, (new_w, new_h))
        if not isinstance(pivot, list) or len(pivot) != 2:
            pivot = [0.5, 0.94]
        pivot_x = float(max(0.0, min(1.0, pivot[0])))
        pivot_y = float(max(0.0, min(1.0, pivot[1])))
        math_angle_deg = self._user_angle_to_math_angle(user_angle_deg)
        self._blit_rotated_about_pivot(
            surface=surface,
            sprite=resized,
            world_pivot=(center_x, center_y),
            local_pivot=(resized.get_width() * pivot_x, resized.get_height() * pivot_y),
            angle_deg=math_angle_deg + angle_offset,
        )

    def _draw_clock_line_hand(
        self,
        surface: pygame.Surface,
        center_x: int,
        center_y: int,
        user_angle_deg: float,
        length: int,
        color: tuple[int, int, int],
        width: int,
    ) -> None:
        angle = math.radians(self._user_angle_to_math_angle(user_angle_deg))
        tip = (center_x + int(math.cos(angle) * length), center_y - int(math.sin(angle) * length))
        pygame.draw.line(surface, color, (center_x, center_y), tip, width=max(1, width))

    def _visual_settings(self) -> dict[str, Any]:
        return self._visual_settings_for_keys(self._style_lookup_keys())

    def _visual_settings_for_keys(self, keys: list[str]) -> dict[str, Any]:
        visuals = self._theme_data.get("gauge_visuals", {})
        if not isinstance(visuals, dict):
            return {}
        merged: dict[str, Any] = {}
        for key in keys:
            gauge_visuals = visuals.get(key, {})
            if isinstance(gauge_visuals, dict):
                merged.update(gauge_visuals)
        return merged

    def _apply_primary_calibration(self, value: float, visuals: dict[str, Any]) -> float:
        calibration = visuals.get("calibration", {})
        if not isinstance(calibration, dict):
            return value
        scale = float(calibration.get("scale", 1.0))
        offset = float(calibration.get("offset", 0.0))
        return (value * scale) + offset

    def _apply_source_calibration(self, source: str, value: float) -> tuple[float, str | None]:
        source_calibration = self._theme_data.get("source_calibration", {})
        if not isinstance(source_calibration, dict):
            return value, None
        calibration = source_calibration.get(source, {})
        if not isinstance(calibration, dict):
            return value, None
        scale = float(calibration.get("scale", 1.0))
        offset = float(calibration.get("offset", 0.0))
        unit = calibration.get("unit")
        unit_override = str(unit) if isinstance(unit, str) and unit else None
        return (value * scale) + offset, unit_override

    def _compute_primary_ratio(
        self,
        value: float,
        spec: dict[str, Any],
        visuals: dict[str, Any],
        source_unit_override: str | None,
        visual_min: float,
        visual_max: float,
    ) -> float:
        ratio_setting = visuals.get("ratio_from_config_range")
        if ratio_setting is None:
            # When a source unit conversion is active (for example C->F), default
            # to visual-range ratio because config min/max may still be in source units.
            use_config_range = source_unit_override is None
        else:
            use_config_range = bool(ratio_setting)

        ratio_min_cfg = visuals.get("ratio_min")
        ratio_max_cfg = visuals.get("ratio_max")
        if ratio_min_cfg is not None and ratio_max_cfg is not None:
            range_min = float(ratio_min_cfg)
            range_max = float(ratio_max_cfg)
        elif use_config_range:
            range_min = float(spec["min_value"])
            range_max = float(spec["max_value"])
        else:
            range_min = visual_min
            range_max = visual_max

        if range_max <= range_min:
            return 0.0
        ratio = (value - range_min) / (range_max - range_min)
        ratio = max(0.0, min(1.0, ratio))
        if bool(visuals.get("invert_value", False)):
            ratio = 1.0 - ratio
        return ratio

    def _draw_background(self, surface: pygame.Surface, cx: int, cy: int, radius: int) -> None:
        dial = self._sprite_cache.get("dial")
        hide_dial_sprite = self._theme_flag_for_gauge("debug_hide_dial_sprite", False)
        if hide_dial_sprite:
            return
        if dial:
            if self._theme_flag_for_gauge("clip_dial_to_mask", False):
                self._blit_masked_dial(surface, dial)
            else:
                self._blit_layer_sprite(surface, dial, layer_name="dial", default_mode="fill")
            return
        palette = self._theme_data.get("fallback_palette", {})
        bg = tuple(palette.get("background", [10, 10, 10]))
        ring = tuple(palette.get("ring", [45, 45, 45]))
        pygame.draw.circle(surface, bg, (cx, cy), radius)
        pygame.draw.circle(surface, ring, (cx, cy), radius, width=max(2, radius // 24))

    def _blit_masked_dial(self, surface: pygame.Surface, dial: pygame.Surface) -> None:
        dial_layer = pygame.Surface((self.layout.width, self.layout.height), pygame.SRCALPHA)
        self._blit_layer_sprite(dial_layer, dial, layer_name="dial", default_mode="fill")
        mask = self._load_gauge_mask_surface()
        if not mask:
            surface.blit(dial_layer, (0, 0))
            return
        resized_mask = pygame.transform.smoothscale(mask, (self.layout.width, self.layout.height))
        # Treat near-white pixels in mask art as the visible area.
        mask_bin = pygame.mask.from_threshold(resized_mask, (255, 255, 255, 255), (96, 96, 96, 255))
        alpha_mask = mask_bin.to_surface(setcolor=(255, 255, 255, 255), unsetcolor=(255, 255, 255, 0))
        clipped = dial_layer.copy()
        clipped.blit(alpha_mask, (0, 0), special_flags=pygame.BLEND_RGBA_MULT)
        surface.blit(clipped, (0, 0))

    def _load_gauge_mask_surface(self) -> pygame.Surface | None:
        gauge_id = self.layout.gauge_id or ""
        overrides = self._theme_data.get("mask_overrides", {})
        mask_files = self._theme_data.get("mask_files", {})
        filename = ""
        if isinstance(overrides, dict):
            gauge_override = overrides.get(gauge_id, {})
            if isinstance(gauge_override, dict):
                filename = str(gauge_override.get("dial_mask", ""))
        if not filename and isinstance(mask_files, dict):
            filename = str(mask_files.get("dial_mask", ""))
        return self._load_theme_sprite(filename) if filename else None

    def _draw_ticks(
        self,
        surface: pygame.Surface,
        cx: int,
        cy: int,
        radius: int,
        spec: dict[str, Any],
        visuals: dict[str, Any],
        tick_key: str | None = None,
    ) -> None:
        palette = self._theme_data.get("fallback_palette", {})
        tick_color_default = tuple(palette.get("ticks", [220, 220, 220]))
        style = self._tick_style_for_specific_key(tick_key) if tick_key else self._tick_style_for_gauge()
        angle_min = float(visuals.get("angle_min_deg", 225.0))
        angle_max = float(visuals.get("angle_max_deg", -45.0))
        visual_min = float(visuals.get("visual_min", spec["min_value"]))
        visual_max = float(visuals.get("visual_max", spec["max_value"]))
        redline_cfg = visuals.get("redline", {})
        self._draw_tick_arc(
            surface=surface,
            center_x=cx,
            center_y=cy,
            radius=radius,
            angle_min=angle_min,
            angle_max=angle_max,
            visual_min=visual_min,
            visual_max=visual_max,
            style=style,
            redline_cfg=redline_cfg,
            default_color=tick_color_default,
            invert_value=bool(visuals.get("invert_value", False)),
        )

    def _tick_style_for_gauge(self) -> dict[str, Any]:
        style_cfg = self._theme_data.get("tick_style", {})
        if not isinstance(style_cfg, dict):
            return {}
        merged: dict[str, Any] = {}
        base = style_cfg.get("_default", {})
        if isinstance(base, dict):
            merged.update(base)
        gauge_id = self.layout.gauge_id or ""
        gauge_style = style_cfg.get(gauge_id, {}) if gauge_id else {}
        if isinstance(gauge_style, dict):
            merged.update(gauge_style)
        return merged

    def _tick_style_for_specific_key(self, gauge_key: str) -> dict[str, Any]:
        style_cfg = self._theme_data.get("tick_style", {})
        if not isinstance(style_cfg, dict):
            return {}
        merged: dict[str, Any] = {}
        base = style_cfg.get("_default", {})
        if isinstance(base, dict):
            merged.update(base)
        gauge_style = style_cfg.get(gauge_key, {})
        if isinstance(gauge_style, dict):
            merged.update(gauge_style)
        return merged

    def _draw_tick_arc(
        self,
        surface: pygame.Surface,
        center_x: int,
        center_y: int,
        radius: int,
        angle_min: float,
        angle_max: float,
        visual_min: float,
        visual_max: float,
        style: dict[str, Any],
        redline_cfg: Any,
        default_color: tuple[int, int, int],
        invert_value: bool = False,
    ) -> None:
        count = max(2, int(style.get("count", 11)))
        outer_ratio = float(style.get("outer_ratio", 0.86))
        length_ratio = float(style.get("length_ratio", 0.14)) * float(style.get("scale", 1.0))
        length_ratio = max(0.02, min(0.5, length_ratio))
        inner_ratio = max(0.0, min(outer_ratio - 0.01, outer_ratio - length_ratio))
        width_scale = float(style.get("width_scale", 1.0))
        line_width = max(1, int((radius // 50) * width_scale))
        for idx in range(count):
            ratio = idx / (count - 1)
            angle_user = self._map_angle(ratio, angle_min, angle_max)
            angle = math.radians(self._user_angle_to_math_angle(angle_user))
            outer = (
                center_x + int(math.cos(angle) * radius * outer_ratio),
                center_y - int(math.sin(angle) * radius * outer_ratio),
            )
            inner = (
                center_x + int(math.cos(angle) * radius * inner_ratio),
                center_y - int(math.sin(angle) * radius * inner_ratio),
            )
            value_ratio = (1.0 - ratio) if invert_value else ratio
            tick_value = visual_min + ((visual_max - visual_min) * value_ratio)
            tick_color = self._tick_color_for_value(default_color, redline_cfg, tick_value, value_ratio)
            pygame.draw.line(surface, tick_color, inner, outer, width=line_width)

    def _theme_flag_for_specific_key(self, key: str, gauge_key: str, default: bool) -> bool:
        value = self._theme_data.get(key, default)
        if isinstance(value, dict):
            if gauge_key in value:
                return bool(value[gauge_key])
            if "_default" in value:
                return bool(value["_default"])
            return default
        return bool(value)

    def _gauge_mode_for_key(self, gauge_key: str) -> str:
        cfg = self._theme_data.get("gauge_modes", {})
        mode = "analog"
        if isinstance(cfg, str):
            mode = cfg
        elif isinstance(cfg, dict):
            raw_mode = cfg.get(gauge_key, cfg.get("_default", "analog"))
            if isinstance(raw_mode, str):
                mode = raw_mode
        mode = mode.strip().lower()
        return "digital" if mode == "digital" else "analog"

    def _digital_render_config(self, gauge_key: str) -> dict[str, Any]:
        cfg = self._theme_data.get("digital_render", {})
        if not isinstance(cfg, dict):
            return {}
        merged: dict[str, Any] = {}
        default_cfg = cfg.get("_default", {})
        if isinstance(default_cfg, dict):
            merged.update(default_cfg)
        gauge_cfg = cfg.get(gauge_key, {})
        if isinstance(gauge_cfg, dict):
            merged.update(gauge_cfg)
        return merged

    def _digital_font(self, size: int, preferred_font: str = "") -> pygame.font.Font:
        font_name = str(preferred_font or "").strip()
        cache_key = f"{font_name}|{size}"
        cached = self._font_cache.get(cache_key)
        if cached:
            return cached
        fallback = self.config["themes"]["font_name"]
        resolved = fallback
        if font_name:
            candidates = [
                Path(font_name),
                Path(self.config["themes"]["theme_root"]) / self.config["themes"]["active"] / font_name,
                Path("assets/fonts") / font_name,
            ]
            for candidate in candidates:
                if candidate.exists():
                    resolved = str(candidate)
                    break
        try:
            font = pygame.font.Font(resolved, size)
        except (OSError, FileNotFoundError):
            font = pygame.font.SysFont("Courier New", size, bold=True)
        self._font_cache[cache_key] = font
        return font

    def _render_digital_gauge(
        self,
        surface: pygame.Surface,
        spec: dict[str, Any],
        value: float,
        ratio: float,
        values: dict[str, float],
        status: dict[str, str],
        display_unit: str,
        visual_min: float,
        visual_max: float,
        redline_cfg: Any,
        invert_value: bool,
    ) -> None:
        gauge_id = self.layout.gauge_id or ""
        cfg = self._digital_render_config(gauge_id)
        palette = self._theme_data.get("digital_palette", {})
        panel_bg = tuple(palette.get("panel_background", [6, 10, 14]))
        panel_border = tuple(palette.get("panel_border", [36, 88, 120]))
        text_color = tuple(palette.get("text", [240, 250, 255]))
        accent_color = tuple(palette.get("accent", [72, 218, 255]))
        ring_fill = tuple(palette.get("bar_fill", [96, 228, 255]))
        ring_empty = tuple(palette.get("bar_empty", [22, 36, 48]))

        center_x = self.layout.width // 2
        center_y = self.layout.height // 2
        radius = int(min(self.layout.width, self.layout.height) * float(cfg.get("panel_radius_ratio", 0.45)))
        radius = max(24, radius)
        show_panel = bool(cfg.get("show_panel", True))
        show_panel_border = bool(cfg.get("show_panel_border", True))
        if show_panel:
            pygame.draw.circle(surface, panel_bg, (center_x, center_y), radius)
        if show_panel_border:
            pygame.draw.circle(surface, panel_border, (center_x, center_y), radius, width=max(2, radius // 40))

        preferred_font = str(cfg.get("font_name", ""))
        label_font = self._digital_font(max(14, int(self.layout.height * float(cfg.get("label_scale", 0.08)))), preferred_font)
        value_font = self._digital_font(max(18, int(self.layout.height * float(cfg.get("value_scale", 0.30)))), preferred_font)
        unit_font = self._digital_font(max(14, int(self.layout.height * float(cfg.get("unit_scale", 0.08)))), preferred_font)
        aux_font = self._digital_font(max(12, int(self.layout.height * float(cfg.get("aux_scale", 0.055)))), preferred_font)

        label_text = label_font.render(str(spec.get("label", "")).upper(), True, accent_color)
        show_primary_label = bool(cfg.get("show_primary_label", True))
        if show_primary_label:
            label_rect = label_text.get_rect(center=(center_x, center_y - int(radius * 0.55)))
            surface.blit(label_text, label_rect)

        decimals = int(cfg.get("value_decimals", 0))
        show_sign = bool(cfg.get("show_sign", False))
        if decimals <= 0:
            value_text = f"{value:+.0f}" if show_sign else f"{value:.0f}"
        else:
            value_text = f"{value:+.{decimals}f}" if show_sign else f"{value:.{decimals}f}"
        show_primary_readout = bool(cfg.get("show_primary_readout", True))
        value_rect = pygame.Rect(center_x, center_y, 0, 0)
        unit_rect = pygame.Rect(center_x, center_y, 0, 0)
        if show_primary_readout:
            value_surface = value_font.render(value_text, True, text_color)
            value_rect = value_surface.get_rect(center=(center_x, center_y - int(radius * 0.06)))
            surface.blit(value_surface, value_rect)

            unit_surface = unit_font.render(display_unit, True, accent_color)
            unit_rect = unit_surface.get_rect(midtop=(center_x, value_rect.bottom))
            surface.blit(unit_surface, unit_rect)

        secondary = self._secondary_label(spec, values_source_map=values)
        show_secondary_readout = bool(cfg.get("show_secondary_readout", True))
        if secondary and show_primary_readout and show_secondary_readout:
            secondary_surface = aux_font.render(secondary, True, text_color)
            secondary_rect = secondary_surface.get_rect(midtop=(center_x, unit_rect.bottom + 4))
            surface.blit(secondary_surface, secondary_rect)

        ring_outer = max(20, int(radius * float(cfg.get("ring_outer_ratio", 0.93))))
        ring_thickness = max(8, int(radius * float(cfg.get("ring_thickness_ratio", 0.18))))
        ring_inner = max(6, ring_outer - ring_thickness)
        ring_segments = max(8, int(cfg.get("ring_segments", 28)))
        ring_gap_ratio = float(cfg.get("ring_gap_ratio", 0.22))
        ring_angle_min = float(cfg.get("ring_angle_min_deg", 225.0))
        ring_angle_max = float(cfg.get("ring_angle_max_deg", -45.0))
        show_primary_ring = bool(cfg.get("show_primary_ring", True))
        if show_primary_ring:
            self._draw_segmented_donut(
                surface=surface,
                center_x=center_x,
                center_y=center_y,
                inner_radius=ring_inner,
                outer_radius=ring_outer,
                ratio=max(0.0, min(1.0, ratio)),
                segments=ring_segments,
                angle_min_deg=ring_angle_min,
                angle_max_deg=ring_angle_max,
                fill_color=ring_fill,
                empty_color=ring_empty,
                gap_ratio=ring_gap_ratio,
                redline_cfg=redline_cfg,
                visual_min=visual_min,
                visual_max=visual_max,
                invert_value=invert_value,
                reverse=bool(cfg.get("reverse_fill", False)),
            )

            show_min_max = bool(cfg.get("show_primary_min_max", True))
            if show_min_max:
                min_surface = aux_font.render(f"{visual_min:.0f}", True, accent_color)
                max_surface = aux_font.render(f"{visual_max:.0f}", True, accent_color)
                min_angle = math.radians(self._user_angle_to_math_angle(ring_angle_min))
                max_angle = math.radians(self._user_angle_to_math_angle(ring_angle_max))
                label_radius = ring_outer + max(8, int(radius * 0.08))
                min_pos = (
                    center_x + int(math.cos(min_angle) * label_radius),
                    center_y - int(math.sin(min_angle) * label_radius),
                )
                max_pos = (
                    center_x + int(math.cos(max_angle) * label_radius),
                    center_y - int(math.sin(max_angle) * label_radius),
                )
                surface.blit(min_surface, min_surface.get_rect(center=min_pos))
                surface.blit(max_surface, max_surface.get_rect(center=max_pos))

        self._draw_digital_sub_gauges(
            surface=surface,
            values=values,
            cfg=cfg,
            accent_color=accent_color,
            text_color=text_color,
            fallback_fill=ring_fill,
            fallback_empty=ring_empty,
            base_font=aux_font,
        )

        self._draw_indicator_elements(surface, values)
        if self._theme_flag("show_odometer", True):
            self._draw_odometer(surface, spec, values)
        if status.get("theme_warning"):
            warning = aux_font.render(status["theme_warning"], True, (255, 200, 0))
            warning_rect = warning.get_rect(center=(center_x, center_y + int(radius * 0.72)))
            surface.blit(warning, warning_rect)

    def _draw_digital_sub_gauges(
        self,
        surface: pygame.Surface,
        values: dict[str, float],
        cfg: dict[str, Any],
        accent_color: tuple[int, int, int],
        text_color: tuple[int, int, int],
        fallback_fill: tuple[int, int, int],
        fallback_empty: tuple[int, int, int],
        base_font: pygame.font.Font,
    ) -> None:
        gauge_id = self.layout.gauge_id or ""
        sub_defs = self._theme_data.get("sub_gauges", {}).get(gauge_id, [])
        if not isinstance(sub_defs, list):
            return
        min_dim = min(self.layout.width, self.layout.height)
        for sub in sub_defs:
            if not isinstance(sub, dict):
                continue
            source = str(sub.get("source", "")).strip()
            if not source:
                continue
            raw_value = float(values.get(source, 0.0))
            if bool(sub.get("apply_source_calibration", True)):
                raw_value, unit_override = self._apply_source_calibration(source, raw_value)
            else:
                unit_override = None
            calib = sub.get("calibration", {})
            scale = float(calib.get("scale", 1.0)) if isinstance(calib, dict) else 1.0
            offset = float(calib.get("offset", 0.0)) if isinstance(calib, dict) else 0.0
            value = (raw_value * scale) + offset
            min_v = float(sub.get("visual_min", 0.0))
            max_v = float(sub.get("visual_max", 1.0))
            ratio = 0.0 if max_v <= min_v else (value - min_v) / (max_v - min_v)
            ratio = max(0.0, min(1.0, ratio))
            if bool(sub.get("invert_value", False)):
                ratio = 1.0 - ratio

            center = sub.get("center_ratio", [0.5, 0.5])
            if not isinstance(center, list) or len(center) != 2:
                center = [0.5, 0.5]
            cx = int(self.layout.width * float(center[0]))
            cy = int(self.layout.height * float(center[1]))
            offset_x = int(sub.get("offset_x_px", 0))
            offset_y = int(sub.get("offset_y_px", 0))
            offset_pair = sub.get("offset_px")
            if isinstance(offset_pair, list) and len(offset_pair) == 2:
                offset_x = int(offset_pair[0])
                offset_y = int(offset_pair[1])
            cx += offset_x
            cy += offset_y

            fill_color = self._resolve_rgb(sub.get("color"), fallback_fill)
            empty_color = self._resolve_rgb(sub.get("empty_color"), fallback_empty)
            style = str(sub.get("digital_style", cfg.get("sub_default_style", "ring"))).strip().lower()

            if style == "hbar":
                bar_w = max(8, int(min_dim * float(sub.get("bar_width_ratio", 0.28))))
                bar_h = max(6, int(min_dim * float(sub.get("bar_height_ratio", 0.05))))
                rect = pygame.Rect(0, 0, bar_w, bar_h)
                rect.center = (cx, cy)
                self._draw_segmented_linear_bar(
                    surface=surface,
                    rect=rect,
                    ratio=ratio,
                    vertical=False,
                    segments=max(4, int(sub.get("bar_segments", 10))),
                    gap_px=max(0, int(sub.get("bar_gap_px", 2))),
                    fill_color=fill_color,
                    empty_color=empty_color,
                    reverse=bool(sub.get("reverse_fill", False)),
                    rainbow=bool(sub.get("rainbow", False)),
                    rainbow_invert=bool(sub.get("rainbow_invert", False)),
                    redline_cfg=sub.get("redline", {}),
                    visual_min=min_v,
                    visual_max=max_v,
                    invert_value=bool(sub.get("invert_value", False)),
                )
            elif style == "vbar":
                bar_w = max(6, int(min_dim * float(sub.get("bar_width_ratio", 0.07))))
                bar_h = max(8, int(min_dim * float(sub.get("bar_height_ratio", 0.30))))
                rect = pygame.Rect(0, 0, bar_w, bar_h)
                rect.center = (cx, cy)
                self._draw_segmented_linear_bar(
                    surface=surface,
                    rect=rect,
                    ratio=ratio,
                    vertical=True,
                    segments=max(4, int(sub.get("bar_segments", 12))),
                    gap_px=max(0, int(sub.get("bar_gap_px", 2))),
                    fill_color=fill_color,
                    empty_color=empty_color,
                    reverse=bool(sub.get("reverse_fill", False)),
                    slant_px=int(min_dim * float(sub.get("bar_slant_ratio", 0.0))),
                    rainbow=bool(sub.get("rainbow", False)),
                    rainbow_invert=bool(sub.get("rainbow_invert", False)),
                    redline_cfg=sub.get("redline", {}),
                    visual_min=min_v,
                    visual_max=max_v,
                    invert_value=bool(sub.get("invert_value", False)),
                )
            else:
                outer = max(8, int(min_dim * float(sub.get("size_ratio", 0.16))))
                thickness = max(4, int(outer * float(sub.get("ring_thickness_ratio", 0.24))))
                self._draw_segmented_donut(
                    surface=surface,
                    center_x=cx,
                    center_y=cy,
                    inner_radius=max(2, outer - thickness),
                    outer_radius=outer,
                    ratio=ratio,
                    segments=max(4, int(sub.get("ring_segments", 14))),
                    angle_min_deg=float(sub.get("angle_min_deg", 210.0)),
                    angle_max_deg=float(sub.get("angle_max_deg", -30.0)),
                    fill_color=fill_color,
                    empty_color=empty_color,
                    gap_ratio=float(sub.get("ring_gap_ratio", 0.22)),
                    reverse=bool(sub.get("reverse_fill", False)),
                    redline_cfg=sub.get("redline", {}),
                    visual_min=min_v,
                    visual_max=max_v,
                    invert_value=bool(sub.get("invert_value", False)),
                )

            show_label = bool(sub.get("show_label", True))
            show_value = bool(sub.get("show_value", False))
            label_text = str(sub.get("label", sub.get("id", ""))).strip().upper()
            unit_text = str(sub.get("unit", unit_override or "")).strip()
            if show_label and label_text:
                label_surface = base_font.render(label_text, True, accent_color)
                label_offset_px = sub.get("label_offset_px")
                if label_offset_px is not None:
                    label_y = cy + int(label_offset_px)
                else:
                    label_y = cy - int(min_dim * float(sub.get("label_offset_ratio", 0.07)))
                surface.blit(label_surface, label_surface.get_rect(center=(cx, label_y)))
            if show_value:
                decimals = int(sub.get("value_decimals", 0))
                value_text = f"{value:.{max(0, decimals)}f}"
                if unit_text:
                    value_text = f"{value_text}{unit_text}"
                value_surface = base_font.render(value_text, True, text_color)
                value_offset_px = sub.get("value_offset_px")
                if value_offset_px is not None:
                    value_y = cy + int(value_offset_px)
                else:
                    value_y = cy + int(min_dim * float(sub.get("value_offset_ratio", 0.07)))
                surface.blit(value_surface, value_surface.get_rect(center=(cx, value_y)))

    def _resolve_rgb(self, raw: Any, fallback: tuple[int, int, int]) -> tuple[int, int, int]:
        if isinstance(raw, list) and len(raw) == 3:
            try:
                return (int(raw[0]), int(raw[1]), int(raw[2]))
            except (TypeError, ValueError):
                return fallback
        return fallback

    def _render_digital_clock(self, surface: pygame.Surface, status: dict[str, str]) -> None:
        cfg = self._digital_render_config("clock")
        palette = self._theme_data.get("digital_palette", {})
        panel_bg = tuple(palette.get("panel_background", [6, 10, 14]))
        panel_border = tuple(palette.get("panel_border", [36, 88, 120]))
        text_color = tuple(palette.get("text", [240, 250, 255]))
        accent_color = tuple(palette.get("accent", [72, 218, 255]))

        center_x = self.layout.width // 2
        center_y = self.layout.height // 2
        radius = int(min(self.layout.width, self.layout.height) * float(cfg.get("panel_radius_ratio", 0.45)))
        radius = max(20, radius)
        pygame.draw.circle(surface, panel_bg, (center_x, center_y), radius)
        pygame.draw.circle(surface, panel_border, (center_x, center_y), radius, width=max(2, radius // 40))

        preferred_font = str(cfg.get("font_name", ""))
        header_font = self._digital_font(max(12, int(self.layout.height * float(cfg.get("label_scale", 0.10)))), preferred_font)
        clock_font = self._digital_font(max(18, int(self.layout.height * float(cfg.get("value_scale", 0.34)))), preferred_font)
        aux_font = self._digital_font(max(12, int(self.layout.height * 0.055)), preferred_font)

        now = datetime.now()
        use_24h = bool(cfg.get("use_24h", False))
        time_label = f"{now:%H:%M}" if use_24h else f"{now:%I:%M}"
        sec_label = f"{now:%S}"

        title = header_font.render("CLOCK", True, accent_color)
        time_surface = clock_font.render(time_label, True, text_color)
        sec_surface = aux_font.render(sec_label, True, accent_color)
        date_surface = aux_font.render(f"{now:%a %b %d}", True, text_color)

        surface.blit(title, title.get_rect(center=(center_x, center_y - int(radius * 0.5))))
        time_rect = time_surface.get_rect(center=(center_x, center_y - int(radius * 0.02)))
        surface.blit(time_surface, time_rect)
        surface.blit(sec_surface, sec_surface.get_rect(midleft=(time_rect.right + 8, time_rect.centery + 6)))
        surface.blit(date_surface, date_surface.get_rect(center=(center_x, center_y + int(radius * 0.35))))

        if status.get("theme_warning"):
            warning = aux_font.render(status["theme_warning"], True, (255, 200, 0))
            warning_rect = warning.get_rect(center=(center_x, center_y + int(radius * 0.75)))
            surface.blit(warning, warning_rect)

    def _draw_segmented_donut(
        self,
        surface: pygame.Surface,
        center_x: int,
        center_y: int,
        inner_radius: int,
        outer_radius: int,
        ratio: float,
        segments: int,
        angle_min_deg: float,
        angle_max_deg: float,
        fill_color: tuple[int, int, int],
        empty_color: tuple[int, int, int],
        gap_ratio: float,
        reverse: bool = False,
        redline_cfg: Any = None,
        visual_min: float = 0.0,
        visual_max: float = 1.0,
        invert_value: bool = False,
    ) -> None:
        if outer_radius <= inner_radius:
            return
        gap = max(0.0, min(0.48, gap_ratio))
        filled = int(round(max(0.0, min(1.0, ratio)) * segments))
        for idx in range(segments):
            start_ratio = idx / segments
            end_ratio = (idx + 1) / segments
            segment_span = end_ratio - start_ratio
            trim = segment_span * gap * 0.5
            start_ratio += trim
            end_ratio -= trim
            if end_ratio <= start_ratio:
                continue
            logical_idx = (segments - 1 - idx) if reverse else idx
            segment_ratio = 0.0 if segments <= 1 else (logical_idx / (segments - 1))
            value_ratio = (1.0 - segment_ratio) if invert_value else segment_ratio
            tick_value = visual_min + ((visual_max - visual_min) * value_ratio)
            active_fill = self._tick_color_for_value(fill_color, redline_cfg, tick_value, value_ratio)
            is_filled = idx >= (segments - filled) if reverse else idx < filled
            color = active_fill if is_filled else empty_color
            self._fill_ring_slice(
                surface=surface,
                center_x=center_x,
                center_y=center_y,
                inner_radius=inner_radius,
                outer_radius=outer_radius,
                start_ratio=start_ratio,
                end_ratio=end_ratio,
                angle_min_deg=angle_min_deg,
                angle_max_deg=angle_max_deg,
                color=color,
            )

    def _fill_ring_slice(
        self,
        surface: pygame.Surface,
        center_x: int,
        center_y: int,
        inner_radius: int,
        outer_radius: int,
        start_ratio: float,
        end_ratio: float,
        angle_min_deg: float,
        angle_max_deg: float,
        color: tuple[int, int, int],
    ) -> None:
        steps = 5
        outer_points: list[tuple[int, int]] = []
        inner_points: list[tuple[int, int]] = []
        for step in range(steps + 1):
            ratio = start_ratio + ((end_ratio - start_ratio) * (step / steps))
            user_angle = self._map_angle(ratio, angle_min_deg, angle_max_deg)
            angle = math.radians(self._user_angle_to_math_angle(user_angle))
            outer_points.append(
                (
                    center_x + int(math.cos(angle) * outer_radius),
                    center_y - int(math.sin(angle) * outer_radius),
                )
            )
            inner_points.append(
                (
                    center_x + int(math.cos(angle) * inner_radius),
                    center_y - int(math.sin(angle) * inner_radius),
                )
            )
        pygame.draw.polygon(surface, color, outer_points + list(reversed(inner_points)))

    def _draw_segmented_linear_bar(
        self,
        surface: pygame.Surface,
        rect: pygame.Rect,
        ratio: float,
        vertical: bool,
        segments: int,
        gap_px: int,
        fill_color: tuple[int, int, int],
        empty_color: tuple[int, int, int],
        reverse: bool = False,
        slant_px: int = 0,
        rainbow: bool = False,
        rainbow_invert: bool = False,
        rainbow_colors: list[tuple[int, int, int]] | None = None,
        redline_cfg: Any = None,
        visual_min: float = 0.0,
        visual_max: float = 1.0,
        invert_value: bool = False,
    ) -> None:
        if rect.width <= 0 or rect.height <= 0:
            return
        segments = max(2, segments)
        gap_px = max(0, gap_px)
        ratio = max(0.0, min(1.0, ratio))
        filled = int(round(ratio * segments))
        if rainbow:
            palette = rainbow_colors or [
                (46, 205, 113),
                (142, 214, 54),
                (255, 206, 64),
                (255, 160, 64),
                (255, 92, 92),
            ]
            if rainbow_invert:
                palette = list(reversed(palette))
        else:
            palette = []
        if vertical:
            segment_h = max(1, int((rect.height - ((segments - 1) * gap_px)) / segments))
            slant_px = int(slant_px)
            usable_w = max(1, rect.width - abs(slant_px))
            slant_base_x = rect.left + max(0, -slant_px)
            for idx in range(segments):
                draw_idx = (segments - 1 - idx) if reverse else idx
                seg_y = rect.top + (draw_idx * (segment_h + gap_px))
                row_ratio = 0.0 if segments <= 1 else (draw_idx / (segments - 1))
                x_offset = int(round((1.0 - row_ratio) * slant_px))
                seg_rect = pygame.Rect(slant_base_x + x_offset, seg_y, usable_w, segment_h)
                is_filled = idx < filled
                logical_idx = (segments - 1 - idx) if reverse else idx
                segment_ratio = 0.0 if segments <= 1 else (logical_idx / (segments - 1))
                value_ratio = (1.0 - segment_ratio) if invert_value else segment_ratio
                tick_value = visual_min + ((visual_max - visual_min) * value_ratio)
                active_fill = self._tick_color_for_value(fill_color, redline_cfg, tick_value, value_ratio)
                if is_filled and rainbow:
                    ratio_idx = logical_idx / max(1, segments - 1)
                    color_idx = int(ratio_idx * (len(palette) - 1))
                    seg_color = palette[color_idx]
                else:
                    seg_color = active_fill if is_filled else empty_color
                pygame.draw.rect(surface, seg_color, seg_rect, border_radius=2)
            return
        segment_w = max(1, int((rect.width - ((segments - 1) * gap_px)) / segments))
        for idx in range(segments):
            draw_idx = (segments - 1 - idx) if reverse else idx
            seg_x = rect.left + (draw_idx * (segment_w + gap_px))
            seg_rect = pygame.Rect(seg_x, rect.top, segment_w, rect.height)
            is_filled = idx < filled
            logical_idx = (segments - 1 - idx) if reverse else idx
            segment_ratio = 0.0 if segments <= 1 else (logical_idx / (segments - 1))
            value_ratio = (1.0 - segment_ratio) if invert_value else segment_ratio
            tick_value = visual_min + ((visual_max - visual_min) * value_ratio)
            active_fill = self._tick_color_for_value(fill_color, redline_cfg, tick_value, value_ratio)
            if is_filled and rainbow:
                ratio_idx = logical_idx / max(1, segments - 1)
                color_idx = int(ratio_idx * (len(palette) - 1))
                seg_color = palette[color_idx]
            else:
                seg_color = active_fill if is_filled else empty_color
            pygame.draw.rect(surface, seg_color, seg_rect, border_radius=2)

    def _tick_color_for_value(
        self,
        default_color: tuple[int, int, int],
        redline_cfg: Any,
        tick_value: float,
        ratio: float,
    ) -> tuple[int, int, int]:
        if not isinstance(redline_cfg, dict):
            return default_color
        red_raw = redline_cfg.get("color", [255, 64, 64])
        red_color = tuple(red_raw) if isinstance(red_raw, list) and len(red_raw) == 3 else (255, 64, 64)
        start_ratio = redline_cfg.get("start_ratio")
        end_ratio = redline_cfg.get("end_ratio")
        if start_ratio is not None:
            start = float(start_ratio)
            end = float(end_ratio) if end_ratio is not None else 1.0
            if start <= ratio <= end:
                return red_color
            return default_color
        start_value = redline_cfg.get("start_value")
        if start_value is None:
            return default_color
        start = float(start_value)
        end = float(redline_cfg.get("end_value", float("inf")))
        if start <= tick_value <= end:
            return red_color
        return default_color

    def _draw_needle(self, surface: pygame.Surface, cx: int, cy: int, radius: int, ratio: float) -> None:
        visuals = self._visual_settings()
        angle_min_deg = float(visuals.get("angle_min_deg", 225.0))
        angle_max_deg = float(visuals.get("angle_max_deg", -45.0))
        angle_deg = self._map_angle(ratio, angle_min_deg, angle_max_deg)
        math_angle_deg = self._user_angle_to_math_angle(angle_deg)
        angle = math.radians(math_angle_deg)
        needle = self._sprite_cache.get("needle")
        if needle:
            gauge_id = self.layout.gauge_id or ""
            render_cfg = self._theme_data.get("sprite_render", {})
            gauge_cfg = render_cfg.get(gauge_id, {})
            needle_cfg = gauge_cfg.get("needle", {})
            scaled = self._scale_primary_needle_sprite(needle, radius, needle_cfg)
            pivot = needle_cfg.get("pivot", [0.5, 0.94])
            if not isinstance(pivot, list) or len(pivot) != 2:
                pivot = [0.5, 0.94]
            pivot_x = float(max(0.0, min(1.0, pivot[0])))
            pivot_y = float(max(0.0, min(1.0, pivot[1])))
            angle_offset = float(needle_cfg.get("angle_offset_deg", -90.0))
            self._blit_rotated_about_pivot(
                surface=surface,
                sprite=scaled,
                world_pivot=(cx, cy),
                local_pivot=(scaled.get_width() * pivot_x, scaled.get_height() * pivot_y),
                angle_deg=math_angle_deg + angle_offset,
            )
            return
        needle_color = tuple(self._theme_data.get("fallback_palette", {}).get("needle", [235, 70, 65]))
        tip = (cx + int(math.cos(angle) * radius * 0.72), cy - int(math.sin(angle) * radius * 0.72))
        pygame.draw.line(surface, needle_color, (cx, cy), tip, width=max(2, radius // 40))
        pygame.draw.circle(surface, needle_color, (cx, cy), max(5, radius // 18))

    def _draw_overlay(self, surface: pygame.Surface, cx: int, cy: int, radius: int) -> None:
        overlay = self._sprite_cache.get("overlay")
        if overlay:
            self._blit_layer_sprite(surface, overlay, layer_name="overlay", default_mode="fill")
            return
        if not self._theme_flag_for_gauge("show_overlay_fallback", True):
            return
        overlay_color = tuple(self._theme_data.get("fallback_palette", {}).get("overlay", [25, 25, 25]))
        pygame.draw.circle(surface, overlay_color, (cx, cy + int(radius * 0.42)), int(radius * 0.21))

    def _scale_primary_needle_sprite(
        self, needle: pygame.Surface, radius: int, needle_cfg: dict[str, Any]
    ) -> pygame.Surface:
        mode = str(needle_cfg.get("mode", "legacy"))
        if mode == "native_scale":
            scale = float(needle_cfg.get("scale", 0.72))
            target = max(8, int(min(self.layout.width, self.layout.height) * scale))
            w, h = needle.get_size()
            if w <= 0 or h <= 0:
                return needle
            if w >= h:
                new_w = target
                new_h = max(1, int((h / w) * target))
            else:
                new_h = target
                new_w = max(1, int((w / h) * target))
            return pygame.transform.smoothscale(needle, (new_w, new_h))
        return pygame.transform.smoothscale(needle, (max(8, radius // 9), int(radius * 0.78)))

    def _draw_text(
        self,
        surface: pygame.Surface,
        spec: dict[str, Any],
        value: float,
        values: dict[str, float],
        status: dict[str, str],
        unit_override: str | None = None,
    ) -> None:
        display_unit = unit_override or str(spec.get("unit", ""))
        label = f"{spec['label']} {value:.1f} {display_unit}"
        text = self._font.render(label, True, (240, 240, 240))
        text_rect = text.get_rect(center=(self.layout.width // 2, int(self.layout.height * 0.78)))
        surface.blit(text, text_rect)

        secondary = self._secondary_label(spec, values_source_map=values)
        if secondary:
            secondary_font = pygame.font.Font(self.config["themes"]["font_name"], max(14, self.layout.height // 28))
            secondary_text = secondary_font.render(secondary, True, (190, 214, 236))
            secondary_rect = secondary_text.get_rect(center=(self.layout.width // 2, int(self.layout.height * 0.14)))
            surface.blit(secondary_text, secondary_rect)

        if status.get("theme_warning"):
            warning_font = pygame.font.Font(self.config["themes"]["font_name"], max(14, self.layout.height // 24))
            warning = warning_font.render(status["theme_warning"], True, (255, 200, 0))
            warning_rect = warning.get_rect(center=(self.layout.width // 2, int(self.layout.height * 0.9)))
            surface.blit(warning, warning_rect)

    def _secondary_label(self, spec: dict[str, Any], values_source_map: dict[str, float]) -> str:
        secondary_source = spec.get("secondary_source")
        if not secondary_source:
            return ""
        secondary_value = float(values_source_map.get(str(secondary_source), 0.0))
        secondary_value, secondary_unit_override = self._apply_source_calibration(str(secondary_source), secondary_value)
        secondary_unit = secondary_unit_override or str(spec.get("secondary_unit", ""))
        secondary_label = str(spec.get("secondary_label", ""))
        if secondary_label:
            return f"{secondary_label} {secondary_value:.0f}{secondary_unit}"
        return f"{secondary_value:.0f}{secondary_unit}"

    def _blit_layer_sprite(
        self,
        surface: pygame.Surface,
        sprite: pygame.Surface,
        layer_name: str,
        default_mode: str = "fill",
    ) -> None:
        gauge_id = self._active_sprite_key()
        render_cfg = self._theme_data.get("sprite_render", {})
        gauge_cfg = render_cfg.get(gauge_id, {})
        layer_cfg = gauge_cfg.get(layer_name, {})
        mode = str(layer_cfg.get("mode", default_mode))
        offset_x, offset_y = self._layer_offset(layer_cfg)

        if mode == "center":
            rect = sprite.get_rect(center=((self.layout.width // 2) + offset_x, (self.layout.height // 2) + offset_y))
            surface.blit(sprite, rect.topleft)
            return

        if mode == "center_scale":
            scale = float(layer_cfg.get("scale", 0.2))
            target = max(4, int(min(self.layout.width, self.layout.height) * scale))
            w, h = sprite.get_size()
            if w <= 0 or h <= 0:
                return
            if w >= h:
                new_w = target
                new_h = max(1, int((h / w) * target))
            else:
                new_h = target
                new_w = max(1, int((w / h) * target))
            resized = pygame.transform.smoothscale(sprite, (new_w, new_h))
            rect = resized.get_rect(center=((self.layout.width // 2) + offset_x, (self.layout.height // 2) + offset_y))
            surface.blit(resized, rect.topleft)
            return

        resized = pygame.transform.smoothscale(sprite, (self.layout.width, self.layout.height))
        surface.blit(resized, (offset_x, offset_y))

    def _layer_offset(self, layer_cfg: dict[str, Any]) -> tuple[int, int]:
        offset_x = int(layer_cfg.get("offset_x_px", 0))
        offset_y = int(layer_cfg.get("offset_y_px", 0))
        offset_px = layer_cfg.get("offset_px")
        if isinstance(offset_px, list) and len(offset_px) == 2:
            offset_x = int(offset_px[0])
            offset_y = int(offset_px[1])
        return offset_x, offset_y

    def _theme_flag(self, key: str, default: bool) -> bool:
        value = self._theme_data.get(key, default)
        return bool(value)

    def _theme_flag_for_gauge(self, key: str, default: bool) -> bool:
        value = self._theme_data.get(key, default)
        if isinstance(value, dict):
            for lookup_key in self._style_lookup_keys():
                if lookup_key in value:
                    return bool(value[lookup_key])
            if "_default" in value:
                return bool(value["_default"])
            return default
        return bool(value)

    def _style_lookup_keys(self) -> list[str]:
        keys: list[str] = []
        gauge_id = self.layout.gauge_id or ""
        if gauge_id:
            keys.append(gauge_id)
        sub_defs = self._theme_data.get("sub_gauges", {}).get(gauge_id, [])
        if isinstance(sub_defs, list):
            for sub in sub_defs:
                if not isinstance(sub, dict):
                    continue
                sub_id = str(sub.get("id", "")).strip()
                if sub_id and sub_id not in keys:
                    keys.append(sub_id)
        return keys

    def _screen_background_color(self) -> tuple[int, int, int, int]:
        color_raw = self._theme_data.get("screen_background", [0, 0, 0])
        if isinstance(color_raw, list) and len(color_raw) >= 3:
            return (int(color_raw[0]), int(color_raw[1]), int(color_raw[2]), 255)
        return (0, 0, 0, 255)

    def _draw_sub_gauges(self, surface: pygame.Surface, values: dict[str, float]) -> None:
        gauge_id = self.layout.gauge_id or ""
        sub_defs = self._theme_data.get("sub_gauges", {}).get(gauge_id, [])
        if not isinstance(sub_defs, list):
            return
        for sub in sub_defs:
            if not isinstance(sub, dict):
                continue
            sub_id = str(sub.get("id", "")).strip()
            source = str(sub.get("source", ""))
            if not source:
                continue
            raw_value = float(values.get(source, 0.0))
            if bool(sub.get("apply_source_calibration", True)):
                raw_value, _ = self._apply_source_calibration(source, raw_value)
            calib = sub.get("calibration", {})
            scale = float(calib.get("scale", 1.0)) if isinstance(calib, dict) else 1.0
            offset = float(calib.get("offset", 0.0)) if isinstance(calib, dict) else 0.0
            value = (raw_value * scale) + offset
            min_v = float(sub.get("visual_min", 0.0))
            max_v = float(sub.get("visual_max", 1.0))
            ratio = 0.0 if max_v <= min_v else (value - min_v) / (max_v - min_v)
            ratio = max(0.0, min(1.0, ratio))
            if bool(sub.get("invert_value", False)):
                ratio = 1.0 - ratio

            angle_min = float(sub.get("angle_min_deg", -90.0))
            angle_max = float(sub.get("angle_max_deg", 90.0))
            angle_deg = self._map_angle(ratio, angle_min, angle_max)
            center = sub.get("center_ratio", [0.5, 0.5])
            if not isinstance(center, list) or len(center) != 2:
                center = [0.5, 0.5]
            center_x = int(self.layout.width * float(center[0]))
            center_y = int(self.layout.height * float(center[1]))
            offset_x = int(sub.get("offset_x_px", 0))
            offset_y = int(sub.get("offset_y_px", 0))
            offset_pair = sub.get("offset_px")
            if isinstance(offset_pair, list) and len(offset_pair) == 2:
                offset_x = int(offset_pair[0])
                offset_y = int(offset_pair[1])
            center_x += offset_x
            center_y += offset_y

            if sub_id and self._theme_flag_for_specific_key("show_ticks", sub_id, False):
                style = self._tick_style_for_specific_key(sub_id)
                radius_ratio = sub.get("tick_radius_ratio")
                if radius_ratio is None:
                    radius_ratio = sub.get("radius_ratio")
                if radius_ratio is None:
                    needle_len_ratio = float(sub.get("needle_length_ratio", 0.12))
                    radius_ratio = max(0.05, needle_len_ratio / 0.72)
                sub_radius = max(8, int(min(self.layout.width, self.layout.height) * float(radius_ratio)))
                sub_redline = sub.get("redline", {})
                tick_color_default = tuple(self._theme_data.get("fallback_palette", {}).get("ticks", [220, 220, 220]))
                self._draw_tick_arc(
                    surface=surface,
                    center_x=center_x,
                    center_y=center_y,
                    radius=sub_radius,
                    angle_min=angle_min,
                    angle_max=angle_max,
                    visual_min=min_v,
                    visual_max=max_v,
                    style=style,
                    redline_cfg=sub_redline,
                    default_color=tick_color_default,
                    invert_value=bool(sub.get("invert_value", False)),
                )

            needle_sprite_name = str(sub.get("needle_sprite", ""))
            needle_surface = self._load_theme_sprite(needle_sprite_name) if needle_sprite_name else None
            if needle_surface:
                self._draw_sub_sprite_needle(surface, needle_surface, sub, center_x, center_y, angle_deg)
            else:
                self._draw_sub_line_needle(surface, sub, center_x, center_y, angle_deg)

    def _draw_indicator_elements(self, surface: pygame.Surface, values: dict[str, float]) -> None:
        gauge_id = self.layout.gauge_id or ""
        indicator_defs = self._theme_data.get("indicator_elements", {}).get(gauge_id, [])
        if not isinstance(indicator_defs, list):
            return
        startup_force_on = bool(values.get("system.indicators_startup_on", 0.0))
        for indicator in indicator_defs:
            if not isinstance(indicator, dict):
                continue
            source = str(indicator.get("source", ""))
            if not source:
                continue
            raw = float(values.get(source, 0.0))
            if bool(indicator.get("apply_source_calibration", True)):
                raw, _ = self._apply_source_calibration(source, raw)
            use_startup_force_on = bool(indicator.get("startup_force_on", True))
            force_on = bool(indicator.get("force_on", False)) or (startup_force_on and use_startup_force_on)
            is_on = force_on or self._indicator_is_on(raw, indicator)
            on_sprite = str(indicator.get("on_sprite", ""))
            off_sprite = str(indicator.get("off_sprite", ""))
            show_when_off = bool(indicator.get("show_when_off", True))
            sprite_name = on_sprite if is_on else off_sprite
            if is_on and (not sprite_name) and off_sprite:
                sprite_name = off_sprite
            if not sprite_name and (not is_on) and not show_when_off:
                continue
            sprite = self._load_theme_sprite(sprite_name) if sprite_name else None
            if sprite:
                self._blit_indicator_sprite(surface, sprite, indicator)
                continue
            self._draw_indicator_shape(surface, indicator, is_on)

    def _indicator_is_on(self, value: float, indicator: dict[str, Any]) -> bool:
        condition = indicator.get("on_condition", {})
        if isinstance(condition, dict) and condition:
            op = str(condition.get("operator", ">")).strip()
            target = float(condition.get("value", 0.0))
            if op == ">":
                return value > target
            if op == ">=":
                return value >= target
            if op == "<":
                return value < target
            if op == "<=":
                return value <= target
            if op == "==":
                return math.isclose(value, target, rel_tol=0.0, abs_tol=1e-6)
            if op == "!=":
                return not math.isclose(value, target, rel_tol=0.0, abs_tol=1e-6)
        threshold = float(indicator.get("on_threshold", 0.0))
        return value >= threshold

    def _blit_indicator_sprite(self, surface: pygame.Surface, sprite: pygame.Surface, indicator: dict[str, Any]) -> None:
        size_ratio = float(indicator.get("size_ratio", 0.12))
        zoom_scale = float(indicator.get("zoom_scale", 1.0))
        size_ratio = max(0.01, size_ratio * max(0.01, zoom_scale))
        target = max(6, int(min(self.layout.width, self.layout.height) * size_ratio))
        w, h = sprite.get_size()
        if w <= 0 or h <= 0:
            return
        if w >= h:
            new_w = target
            new_h = max(1, int((h / w) * target))
        else:
            new_h = target
            new_w = max(1, int((w / h) * target))
        resized = pygame.transform.smoothscale(sprite, (new_w, new_h))
        center = indicator.get("center_ratio", [0.5, 0.5])
        if not isinstance(center, list) or len(center) != 2:
            center = [0.5, 0.5]
        offset_x = int(indicator.get("offset_x_px", 0))
        offset_y = int(indicator.get("offset_y_px", 0))
        offset_pair = indicator.get("offset_px")
        if isinstance(offset_pair, list) and len(offset_pair) == 2:
            offset_x = int(offset_pair[0])
            offset_y = int(offset_pair[1])
        cx = int(self.layout.width * float(center[0])) + offset_x
        cy = int(self.layout.height * float(center[1])) + offset_y
        rect = resized.get_rect(center=(cx, cy))
        surface.blit(resized, rect.topleft)

    def _draw_indicator_shape(self, surface: pygame.Surface, indicator: dict[str, Any], is_on: bool) -> None:
        shape = str(indicator.get("shape", "")).strip().lower()
        if shape not in {"arrow_left", "arrow_right"}:
            return
        if (not is_on) and (not bool(indicator.get("show_when_off", True))):
            return

        center = indicator.get("center_ratio", [0.5, 0.5])
        if not isinstance(center, list) or len(center) != 2:
            center = [0.5, 0.5]
        offset_x = int(indicator.get("offset_x_px", 0))
        offset_y = int(indicator.get("offset_y_px", 0))
        offset_pair = indicator.get("offset_px")
        if isinstance(offset_pair, list) and len(offset_pair) == 2:
            offset_x = int(offset_pair[0])
            offset_y = int(offset_pair[1])
        cx = int(self.layout.width * float(center[0])) + offset_x
        cy = int(self.layout.height * float(center[1])) + offset_y

        indicator_size = max(6, int(min(self.layout.width, self.layout.height) * float(indicator.get("size_ratio", 0.08))))
        on_color = self._resolve_rgb(indicator.get("on_color"), (255, 208, 64))
        off_color = self._resolve_rgb(indicator.get("off_color"), (64, 52, 24))
        color = on_color if is_on else off_color

        half_w = max(4, indicator_size)
        half_h = max(3, int(indicator_size * 0.5))
        tail_w = max(2, int(indicator_size * 0.35))
        if shape == "arrow_left":
            points = [
                (cx - half_w, cy),
                (cx - tail_w, cy - half_h),
                (cx - tail_w, cy - int(half_h * 0.45)),
                (cx + half_w, cy - int(half_h * 0.45)),
                (cx + half_w, cy + int(half_h * 0.45)),
                (cx - tail_w, cy + int(half_h * 0.45)),
                (cx - tail_w, cy + half_h),
            ]
        else:
            points = [
                (cx + half_w, cy),
                (cx + tail_w, cy - half_h),
                (cx + tail_w, cy - int(half_h * 0.45)),
                (cx - half_w, cy - int(half_h * 0.45)),
                (cx - half_w, cy + int(half_h * 0.45)),
                (cx + tail_w, cy + int(half_h * 0.45)),
                (cx + tail_w, cy + half_h),
            ]
        pygame.draw.polygon(surface, color, points)

    def _load_theme_sprite(self, filename: str) -> pygame.Surface | None:
        if not filename:
            return None
        if filename in self._theme_sprite_cache:
            return self._theme_sprite_cache[filename]
        sprite_theme_root = Path(self.config["themes"]["gauge_asset_root"]) / self.config["themes"]["active"]
        sprite_path = sprite_theme_root / filename
        if not sprite_path.exists():
            return None
        surface = self._load_sprite_surface(sprite_path)
        self._theme_sprite_cache[filename] = surface
        return surface

    def _draw_sub_sprite_needle(
        self,
        surface: pygame.Surface,
        needle_surface: pygame.Surface,
        sub_cfg: dict[str, Any],
        center_x: int,
        center_y: int,
        angle_deg: float,
    ) -> None:
        scale = float(sub_cfg.get("needle_scale", 0.08))
        target = max(6, int(min(self.layout.width, self.layout.height) * scale))
        w, h = needle_surface.get_size()
        if w <= 0 or h <= 0:
            return
        if w >= h:
            new_w = target
            new_h = max(1, int((h / w) * target))
        else:
            new_h = target
            new_w = max(1, int((w / h) * target))
        resized = pygame.transform.smoothscale(needle_surface, (new_w, new_h))
        pivot = sub_cfg.get("needle_pivot", [0.5, 0.9])
        if not isinstance(pivot, list) or len(pivot) != 2:
            pivot = [0.5, 0.9]
        pivot_x = float(max(0.0, min(1.0, pivot[0])))
        pivot_y = float(max(0.0, min(1.0, pivot[1])))
        self._blit_rotated_about_pivot(
            surface=surface,
            sprite=resized,
            world_pivot=(center_x, center_y),
            local_pivot=(resized.get_width() * pivot_x, resized.get_height() * pivot_y),
            angle_deg=self._user_angle_to_math_angle(angle_deg) - 90.0,
        )

    def _draw_sub_line_needle(
        self,
        surface: pygame.Surface,
        sub_cfg: dict[str, Any],
        center_x: int,
        center_y: int,
        angle_deg: float,
    ) -> None:
        length_ratio = float(sub_cfg.get("needle_length_ratio", 0.08))
        length = int(min(self.layout.width, self.layout.height) * length_ratio)
        angle = math.radians(self._user_angle_to_math_angle(angle_deg))
        tip = (center_x + int(math.cos(angle) * length), center_y - int(math.sin(angle) * length))
        color_raw = sub_cfg.get("needle_color", [255, 80, 70])
        color = tuple(color_raw) if isinstance(color_raw, list) and len(color_raw) == 3 else (255, 80, 70)
        width = int(max(1, sub_cfg.get("needle_width", 2)))
        pygame.draw.line(surface, color, (center_x, center_y), tip, width=width)

    def _draw_odometer(self, surface: pygame.Surface, spec: dict[str, Any], values: dict[str, float]) -> None:
        odometer_source = spec.get("odometer_source")
        if not odometer_source:
            return
        gauge_id = self.layout.gauge_id or ""
        odometer_cfg = self._theme_data.get("odometer_render", {}).get(gauge_id, {})
        odometer_value = float(values.get(str(odometer_source), 0.0))
        decimals = int(spec.get("odometer_decimals", 1))
        label = str(spec.get("odometer_label", "ODO"))
        unit = str(spec.get("odometer_unit", "mi"))
        text_value = f"{odometer_value:.{decimals}f}"
        include_label = bool(odometer_cfg.get("include_label", True))
        include_unit = bool(odometer_cfg.get("include_unit", True))
        prefix_parts: list[str] = []
        if include_label and label:
            prefix_parts.append(label)
        suffix_parts: list[str] = []
        if include_unit and unit:
            suffix_parts.append(unit)

        base_font_size = max(12, self.layout.height // 26)
        if "font_scale" in odometer_cfg:
            base_font_size = max(10, int(self.layout.height * float(odometer_cfg["font_scale"])))
        max_width_ratio = float(odometer_cfg.get("max_width_ratio", 0.6))
        max_width_px = int(self.layout.width * max(0.1, min(0.98, max_width_ratio)))
        text_color_raw = odometer_cfg.get("text_color", [238, 245, 250])
        text_color = tuple(text_color_raw) if isinstance(text_color_raw, list) and len(text_color_raw) == 3 else (238, 245, 250)
        decimal_color_raw = odometer_cfg.get("decimal_color", [232, 72, 72])
        decimal_color = (
            tuple(decimal_color_raw)
            if isinstance(decimal_color_raw, list) and len(decimal_color_raw) == 3
            else (232, 72, 72)
        )
        show_decimal_separator = bool(odometer_cfg.get("show_decimal_separator", True))
        font_size = base_font_size
        font = self._odometer_font(odometer_cfg, font_size)
        segments = self._build_odometer_segments(
            font=font,
            text_value=text_value,
            decimals=decimals,
            prefix_parts=prefix_parts,
            suffix_parts=suffix_parts,
            text_color=text_color,
            decimal_color=decimal_color,
            show_decimal_separator=show_decimal_separator,
        )
        total_width = self._segments_total_width(segments)
        while total_width > max_width_px and font_size > 10:
            font_size -= 1
            font = self._odometer_font(odometer_cfg, font_size)
            segments = self._build_odometer_segments(
                font=font,
                text_value=text_value,
                decimals=decimals,
                prefix_parts=prefix_parts,
                suffix_parts=suffix_parts,
                text_color=text_color,
                decimal_color=decimal_color,
                show_decimal_separator=show_decimal_separator,
            )
            total_width = self._segments_total_width(segments)
        y_ratio = float(odometer_cfg.get("y_ratio", 0.9))
        center_y = int(self.layout.height * y_ratio)
        total_height = max((segment.get_height() for segment in segments), default=0)
        start_x = (self.layout.width - total_width) // 2
        start_y = center_y - (total_height // 2)
        pad_x = max(8, self.layout.width // 80)
        pad_y = max(4, self.layout.height // 120)
        box = pygame.Rect(
            start_x - pad_x,
            start_y - pad_y,
            total_width + (pad_x * 2),
            total_height + (pad_y * 2),
        )
        if bool(odometer_cfg.get("show_box", True)):
            box_color_raw = odometer_cfg.get("box_color", [8, 14, 20])
            box_border_raw = odometer_cfg.get("box_border", [96, 138, 168])
            box_color = tuple(box_color_raw) if isinstance(box_color_raw, list) and len(box_color_raw) == 3 else (8, 14, 20)
            box_border = tuple(box_border_raw) if isinstance(box_border_raw, list) and len(box_border_raw) == 3 else (96, 138, 168)
            pygame.draw.rect(surface, box_color, box, border_radius=6)
            pygame.draw.rect(surface, box_border, box, width=1, border_radius=6)
        draw_x = start_x
        for segment in segments:
            surface.blit(segment, (draw_x, center_y - (segment.get_height() // 2)))
            draw_x += segment.get_width() + 3

    def _odometer_font(self, odometer_cfg: dict[str, Any], size: int) -> pygame.font.Font:
        font_name = str(odometer_cfg.get("font_name", "")).strip()
        if font_name:
            return self._digital_font(size, preferred_font=font_name)
        if bool(odometer_cfg.get("use_sys_font", False)):
            family = str(odometer_cfg.get("font_family", "Arial"))
            return pygame.font.SysFont(family, size, bold=False)
        return pygame.font.Font(self.config["themes"]["font_name"], size)

    def _build_odometer_segments(
        self,
        font: pygame.font.Font,
        text_value: str,
        decimals: int,
        prefix_parts: list[str],
        suffix_parts: list[str],
        text_color: tuple[int, int, int],
        decimal_color: tuple[int, int, int],
        show_decimal_separator: bool,
    ) -> list[pygame.Surface]:
        segments: list[pygame.Surface] = []
        for part in prefix_parts:
            segments.append(font.render(part, True, text_color))
        if decimals > 0 and "." in text_value:
            int_part, frac_part = text_value.split(".", 1)
            segments.append(font.render(int_part, True, text_color))
            frac_text = f".{frac_part}" if show_decimal_separator else frac_part
            segments.append(font.render(frac_text, True, decimal_color))
        else:
            segments.append(font.render(text_value, True, text_color))
        for part in suffix_parts:
            segments.append(font.render(part, True, text_color))
        return segments

    def _segments_total_width(self, segments: list[pygame.Surface]) -> int:
        if not segments:
            return 0
        return sum(segment.get_width() for segment in segments) + (3 * (len(segments) - 1))

    def _map_angle(self, ratio: float, angle_start_deg: float, angle_end_deg: float) -> float:
        ratio = max(0.0, min(1.0, ratio))
        start = angle_start_deg % 360.0
        end = angle_end_deg % 360.0
        clockwise_span = (end - start) % 360.0
        return (start + (clockwise_span * ratio)) % 360.0

    def _user_angle_to_math_angle(self, user_angle_deg: float) -> float:
        # User convention: 0 is down, increases clockwise.
        # Math convention for trig: 0 is right, increases counter-clockwise.
        return (270.0 - user_angle_deg) % 360.0

    def _blit_rotated_about_pivot(
        self,
        surface: pygame.Surface,
        sprite: pygame.Surface,
        world_pivot: tuple[int, int],
        local_pivot: tuple[float, float],
        angle_deg: float,
    ) -> None:
        initial_rect = sprite.get_rect(
            topleft=(world_pivot[0] - local_pivot[0], world_pivot[1] - local_pivot[1])
        )
        offset = pygame.math.Vector2(world_pivot) - initial_rect.center
        rotated_offset = offset.rotate(-angle_deg)
        rotated_center = (world_pivot[0] - rotated_offset.x, world_pivot[1] - rotated_offset.y)
        rotated = pygame.transform.rotate(sprite, angle_deg)
        rotated_rect = rotated.get_rect(center=rotated_center)
        surface.blit(rotated, rotated_rect.topleft)
