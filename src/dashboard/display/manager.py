from __future__ import annotations

from dataclasses import dataclass, replace
import json
import os
import re
import subprocess
import traceback
from pathlib import Path
from typing import Any
from typing import Callable

import pygame

from dashboard.screens.gauge_screen import GaugeScreen
from dashboard.screens.layout import DisplayLayout
from dashboard.screens.map_screen import MapScreen
from dashboard.screens.menu_overlay import MenuOverlayController
from dashboard.screens.warning_overlay import WarningOverlayScreen
from dashboard.theme_loader import apply_center_dial_persisted
from dashboard.theme_loader import load_theme_data


@dataclass
class ScreenBinding:
    layout: DisplayLayout
    renderer: GaugeScreen | None


@dataclass
class WindowBinding:
    layout: DisplayLayout
    binding: ScreenBinding
    map_overlay: MapScreen
    window: Any
    renderer: Any


class DisplayManager:
    def __init__(
        self,
        config: dict[str, Any],
        config_path: Path,
        command_registry: dict[str, Callable[[dict[str, Any]], str | None]],
        on_runtime_refresh: Callable[[], None] | None = None,
    ) -> None:
        self._config = config
        self._config_path = config_path
        self._command_registry = command_registry
        self._on_runtime_refresh = on_runtime_refresh
        self._system_view_rotation_deg = self._resolve_system_display_rotation_deg()
        self._view_rotation_flip_hv = self._system_view_rotation_deg == 180
        self._theme_data = self._load_theme_data()
        self._layouts = self._build_layouts()
        self._apply_responsive_canvas_layout()
        self._log_view_rotation_startup()
        self._menu_display_id = self._resolve_menu_display_id()
        self._menu_layout = self._resolve_menu_layout()
        self._menu_render_on_oled = self._resolve_menu_render_on_oled()
        self._alert_display_id = self._resolve_alert_display_id()
        self._alert_layout = self._resolve_alert_layout()
        self._menu_overlay = MenuOverlayController(
            layout=self._menu_layout,
            config=self._config,
            config_path=self._config_path,
            command_registry=self._command_registry,
            on_runtime_refresh=self._on_runtime_refresh,
        )
        self._warning_overlay = WarningOverlayScreen(layout=self._alert_layout, config=self._config)
        self._bindings = self._build_bindings()
        self._map_overlays = self._build_map_overlays()
        self._render_mode = self._resolve_render_mode()
        self._window_bindings: dict[str, WindowBinding] = {}
        self._window_ids: set[int] = set()
        self._interaction_window_ids: set[int] = set()
        self._focused_window_id: int | None = None
        self._root_surface: pygame.Surface | None = None
        self._sdl2_video: Any | None = None
        self._sdl2_texture: Any | None = None
        self._startup_placeholder_drawn = False
        self._logical_size: tuple[int, int] = (640, 480)
        self._physical_size: tuple[int, int] = (640, 480)
        self._composite_surface: pygame.Surface | None = None

        if self._render_mode == "multi_window" and not self._init_multi_window_mode():
            distinct_monitors = {
                int(layout.monitor_index)
                for layout in self._layouts
                if layout.monitor_index is not None
            }
            if len(distinct_monitors) > 1:
                raise RuntimeError(
                    "multi_window init failed but config uses multiple monitor_index values. "
                    "Pygame SDL KMSDRM usually cannot create multiple windows (one per HDMI). "
                    "Use display_render_mode: single_window and disable extra displays, or run under X11."
                )
            self._render_mode = "single_window"
        if self._render_mode == "single_window":
            self._init_single_window_mode()
        self._render_startup_placeholder()

    def _layout_signature(self, layouts: list[DisplayLayout]) -> tuple[tuple[Any, ...], ...]:
        return tuple(
            (
                layout.display_id,
                layout.shape,
                int(layout.width),
                int(layout.height),
                int(layout.x),
                int(layout.y),
                int(layout.monitor_index) if layout.monitor_index is not None else None,
                bool(layout.windowed),
                bool(layout.fullscreen),
                layout.view_rotation_deg,
            )
            for layout in layouts
        )

    def _resolve_startup_placeholder_path(self) -> Path | None:
        system_cfg = self._config.get("system", {})
        configured = str(system_cfg.get("startup_placeholder_image", "assets/bootscreen/boot2.png")).strip()
        if not configured:
            return None
        candidate = Path(configured)
        if candidate.exists():
            return candidate
        if candidate.is_absolute():
            return None
        config_relative = self._config_path.parent / candidate
        if config_relative.exists():
            return config_relative
        return None

    def _resolve_system_display_rotation_deg(self) -> int:
        raw = self._config.get("system", {}).get("display_rotation_deg", 0)
        try:
            deg = int(raw) % 360
        except (TypeError, ValueError):
            return 0
        if deg not in (0, 180):
            print(f"[display] display_rotation_deg={raw!r} unsupported; use 0 or 180", flush=True)
            return 0
        return deg

    def _optional_view_rotation_deg(self, resolved: dict[str, Any]) -> int | None:
        if "view_rotation_deg" not in resolved:
            return None
        raw = resolved["view_rotation_deg"]
        try:
            deg = int(raw) % 360
        except (TypeError, ValueError):
            print(f"[display] displays[{resolved.get('id', '?')}].view_rotation_deg invalid; ignoring", flush=True)
            return None
        if deg not in (0, 180):
            print(f"[display] displays[{resolved.get('id', '?')}].view_rotation_deg must be 0 or 180; ignoring", flush=True)
            return None
        return deg

    def _effective_view_rotation_deg(self, layout: DisplayLayout) -> int:
        if layout.view_rotation_deg is not None:
            return int(layout.view_rotation_deg) % 360
        return self._system_view_rotation_deg

    def _flip_surface_for_layout(self, layout: DisplayLayout, surface: pygame.Surface) -> pygame.Surface:
        if self._effective_view_rotation_deg(layout) != 180:
            return surface
        src = surface.copy()
        return pygame.transform.flip(src, True, True)

    def _should_flip_full_frame_for_uniform_rotation(self) -> bool:
        if not self._layouts:
            return False
        first = self._effective_view_rotation_deg(self._layouts[0])
        if first != 180:
            return False
        return all(self._effective_view_rotation_deg(layout) == first for layout in self._layouts)

    def _flip_surface_hv_inplace(self, surface: pygame.Surface) -> None:
        src = surface.copy()
        surface.blit(pygame.transform.flip(src, True, True), (0, 0))

    def _log_view_rotation_startup(self) -> None:
        if not self._layouts:
            return
        degs = {self._effective_view_rotation_deg(layout) for layout in self._layouts}
        if len(degs) == 1:
            d = next(iter(degs))
            print(
                f"[display] system display_rotation_deg={self._system_view_rotation_deg}; "
                f"all panels effective {d}°",
                flush=True,
            )
        else:
            parts = [f"{layout.display_id}={self._effective_view_rotation_deg(layout)}°" for layout in self._layouts]
            print("[display] mixed view_rotation_deg: " + ", ".join(parts), flush=True)

    def _render_startup_placeholder(self) -> None:
        if self._startup_placeholder_drawn:
            return
        started_ms = pygame.time.get_ticks()
        print("[startup] placeholder render begin", flush=True)
        image_path = self._resolve_startup_placeholder_path()
        if image_path is None:
            elapsed = max(0.0, (pygame.time.get_ticks() - started_ms) / 1000.0)
            print(f"[startup] placeholder render skipped (no image) after {elapsed:.3f}s", flush=True)
            return
        try:
            loaded = pygame.image.load(str(image_path))
            try:
                image = loaded.convert_alpha()
            except pygame.error:
                image = loaded
        except Exception:
            elapsed = max(0.0, (pygame.time.get_ticks() - started_ms) / 1000.0)
            print(f"[startup] placeholder render failed to load after {elapsed:.3f}s", flush=True)
            return

        if self._render_mode == "multi_window" and self._sdl2_texture is not None:
            for binding in self._window_bindings.values():
                scaled = pygame.transform.smoothscale(image, binding.layout.size)
                scaled = self._flip_surface_for_layout(binding.layout, scaled)
                texture = self._sdl2_texture.from_surface(binding.renderer, scaled)
                binding.renderer.draw_color = (0, 0, 0, 255)
                binding.renderer.clear()
                binding.renderer.blit(texture)
                binding.renderer.present()
            self._startup_placeholder_drawn = True
            elapsed = max(0.0, (pygame.time.get_ticks() - started_ms) / 1000.0)
            print(f"[startup] placeholder render done after {elapsed:.3f}s", flush=True)
            return

        if self._root_surface is not None:
            scaled = pygame.transform.smoothscale(image, self._logical_size)
            if self._composite_surface is not None:
                self._composite_surface.blit(scaled, (0, 0))
                out = pygame.transform.smoothscale(self._composite_surface, self._physical_size)
                self._root_surface.blit(out, (0, 0))
            else:
                self._root_surface.blit(scaled, (0, 0))
            if self._should_flip_full_frame_for_uniform_rotation():
                self._flip_surface_hv_inplace(self._root_surface)
            if str(os.environ.get("HEADLESS", "")).strip() != "1":
                pygame.display.flip()
            self._startup_placeholder_drawn = True
            elapsed = max(0.0, (pygame.time.get_ticks() - started_ms) / 1000.0)
            print(f"[startup] placeholder render done after {elapsed:.3f}s", flush=True)

    def _load_theme_data(self) -> dict[str, Any]:
        theme_root = Path(self._config["themes"]["theme_root"])
        theme_name = self._config["themes"]["active"]
        base = load_theme_data(theme_root=theme_root, theme_name=theme_name)
        raw = self._config.get("system", {}).get("persisted_center_dial_mode")
        p: str | None = None
        if isinstance(raw, str):
            pl = raw.strip().lower()
            if pl in ("tach", "clock"):
                p = pl
        return apply_center_dial_persisted(base, p)

    def _resolve_render_mode(self) -> str:
        system_cfg = self._config.get("system", {})
        env_mode = str(os.environ.get("DISPLAY_RENDER_MODE", "")).strip().lower()
        cfg_mode = str(system_cfg.get("display_render_mode", "single_window")).strip().lower()
        mode = env_mode or cfg_mode or "single_window"
        if mode not in {"single_window", "multi_window"}:
            return "single_window"
        return mode

    def _compose_child_surface(
        self,
        layout: DisplayLayout,
        renderer: GaugeScreen,
        map_overlay: MapScreen,
        values: dict[str, float],
        status: dict[str, str],
    ) -> pygame.Surface:
        child = pygame.Surface(layout.size, pygame.SRCALPHA)
        renderer.render(child, values=values, status=status)
        if self._map_overlay_visible(layout):
            overlay = pygame.Surface(layout.size, pygame.SRCALPHA)
            map_overlay.render(overlay, values=values, status=status)
            child.blit(overlay, (0, 0))
        if self._menu_gauge_overlay_visible(layout):
            overlay = pygame.Surface(layout.size, pygame.SRCALPHA)
            self._menu_overlay.render(overlay, status=status)
            child.blit(overlay, (0, 0))
        if self._warning_overlay_visible(layout, values):
            overlay = pygame.Surface(layout.size, pygame.SRCALPHA)
            active_count = int(values.get("arduino.fault_latched_count", values.get("arduino.fault_active_count", 0.0)))
            headline = str(status.get("fault_headline", "Fault detected"))
            self._warning_overlay.render(overlay, active_count=active_count, headline=headline)
            child.blit(overlay, (0, 0))
        return child

    def _single_window_fit_desktop(self) -> bool:
        if str(os.environ.get("HEADLESS", "")).strip() == "1":
            return False
        return bool(self._config.get("system", {}).get("single_window_fit_desktop", False))

    def _fit_desktop_window_size(self, logical: tuple[int, int]) -> tuple[int, int]:
        lw, lh = logical
        system_cfg = self._config.get("system", {})
        margin = int(system_cfg.get("single_window_fit_margin_px", 64))
        try:
            sizes = pygame.display.get_desktop_sizes()
            if not sizes:
                return logical
            desk_w, desk_h = int(sizes[0][0]), int(sizes[0][1])
        except Exception:
            return logical
        avail_w = max(320, desk_w - margin)
        avail_h = max(240, desk_h - margin)
        scale = min(avail_w / max(1, lw), avail_h / max(1, lh))
        if scale >= 1.0:
            return logical
        dw = max(1, int(round(lw * scale)))
        dh = max(1, int(round(lh * scale)))
        return (dw, dh)

    def _init_single_window_mode(self) -> None:
        self._logical_size = self._canvas_size_from_layouts()
        self._physical_size = self._logical_size
        self._composite_surface = None
        if str(os.environ.get("HEADLESS", "")).strip() == "1":
            self._root_surface = pygame.Surface(self._logical_size, pygame.SRCALPHA)
            return
        if self._single_window_fit_desktop():
            skip_fit = len(self._layouts) == 1 and self._layouts[0].fullscreen
            if not skip_fit:
                fit = self._fit_desktop_window_size(self._logical_size)
                if fit != self._logical_size:
                    self._physical_size = fit
                    self._composite_surface = pygame.Surface(self._logical_size)
        # X11 desktop can be narrower than our layout canvas (e.g. 2160×1080 splitter while
        # YAML lays out 2880×1080 for three panels). SDL may still report a full-size surface,
        # so use pygame.display.get_desktop_sizes() and render via composite + smoothscale.
        if str(os.environ.get("HEADLESS", "")).strip() != "1":
            try:
                sizes = pygame.display.get_desktop_sizes()
                if sizes and len(sizes[0]) >= 2:
                    dw, dh = int(sizes[0][0]), int(sizes[0][1])
                    if dw < self._logical_size[0] or dh < self._logical_size[1]:
                        self._physical_size = (
                            min(dw, self._logical_size[0]),
                            min(dh, self._logical_size[1]),
                        )
                        if self._composite_surface is None:
                            self._composite_surface = pygame.Surface(self._logical_size)
                        print(
                            f"[display] desktop {dw}x{dh} < logical {self._logical_size[0]}x{self._logical_size[1]}; "
                            f"window {self._physical_size[0]}x{self._physical_size[1]} + composite",
                            flush=True,
                        )
            except Exception:
                pass
        if self._composite_surface is None and (
            self._physical_size[0] < self._logical_size[0] or self._physical_size[1] < self._logical_size[1]
        ):
            self._composite_surface = pygame.Surface(self._logical_size)
        flags = 0
        # In single-window hardware mode, respect display fullscreen intent to
        # avoid a decorated desktop window/titlebar on the dash screen.
        all_fullscreen = bool(self._layouts) and all(layout.fullscreen for layout in self._layouts)
        if all_fullscreen and not self._single_window_fit_desktop():
            # SDL fullscreen mode switches can stall HDMI bring-up on some Pi/X11
            # paths. Use borderless full-window mode by default for faster first paint.
            use_legacy_fullscreen = bool(self._config.get("system", {}).get("legacy_fullscreen_mode", False))
            if use_legacy_fullscreen:
                flags |= pygame.FULLSCREEN
            else:
                flags |= pygame.NOFRAME
                os.environ.setdefault("SDL_VIDEO_WINDOW_POS", "0,0")
        started = pygame.time.get_ticks()
        print(
            f"[startup] pygame.display.set_mode begin flags={flags} "
            f"logical={self._logical_size[0]}x{self._logical_size[1]} "
            f"physical={self._physical_size[0]}x{self._physical_size[1]}",
            flush=True,
        )
        self._root_surface = pygame.display.set_mode(self._physical_size, flags)
        # SDL sometimes returns a surface size different from what we requested.
        if str(os.environ.get("HEADLESS", "")).strip() != "1":
            actual_w, actual_h = self._root_surface.get_size()
            if actual_w != self._logical_size[0] or actual_h != self._logical_size[1]:
                if self._composite_surface is None:
                    self._composite_surface = pygame.Surface(self._logical_size)
                self._physical_size = (actual_w, actual_h)
                print(
                    f"[display] surface {actual_w}x{actual_h} vs logical {self._logical_size[0]}x{self._logical_size[1]} "
                    f"(composite scaling)",
                    flush=True,
                )
        elapsed = max(0.0, (pygame.time.get_ticks() - started) / 1000.0)
        print(f"[startup] pygame.display.set_mode end after {elapsed:.3f}s", flush=True)
        pygame.display.set_caption(self._config["system"]["window_title"])

    def _init_multi_window_mode(self) -> bool:
        try:
            from pygame._sdl2 import video as sdl2_video
        except Exception:
            print("[display] multi_window unavailable: pygame._sdl2.video import failed")
            return False
        self._sdl2_video = sdl2_video
        self._sdl2_texture = sdl2_video.Texture
        title_root = str(self._config["system"].get("window_title", "El Camino Dash"))
        system_cfg = self._config.get("system", {})
        borderless = bool(system_cfg.get("multi_window_borderless", True))
        for binding in self._bindings:
            try:
                layout = binding.layout
                os.environ["SDL_VIDEO_WINDOW_POS"] = f"{layout.x},{layout.y}"
                wkwargs: dict[str, Any] = {"fullscreen": False}
                if borderless:
                    wkwargs["borderless"] = True
                try:
                    window = sdl2_video.Window(
                        title=f"{title_root} [{layout.display_id}]",
                        size=layout.size,
                        position=(layout.x, layout.y),
                        **wkwargs,
                    )
                except TypeError:
                    wkwargs.pop("borderless", None)
                    window = sdl2_video.Window(
                        title=f"{title_root} [{layout.display_id}]",
                        size=layout.size,
                        position=(layout.x, layout.y),
                        **wkwargs,
                    )
                requested_monitor = layout.monitor_index
                if requested_monitor is not None and hasattr(window, "display_index"):
                    try:
                        # Assign monitor before entering fullscreen for deterministic placement.
                        window.display_index = int(requested_monitor)
                    except Exception:
                        pass
                if layout.fullscreen:
                    try:
                        window.set_fullscreen(True)
                    except Exception:
                        pass
                elif layout.windowed and hasattr(window, "set_windowed"):
                    try:
                        window.set_windowed()
                    except Exception:
                        pass
                resolved_monitor = getattr(window, "display_index", None)
                print(
                    "[display] id={id} requested_monitor={req} resolved_monitor={res} "
                    "fullscreen={full} size={w}x{h}".format(
                        id=layout.display_id,
                        req=requested_monitor,
                        res=resolved_monitor,
                        full=layout.fullscreen,
                        w=layout.width,
                        h=layout.height,
                    )
                )
                # Renderer.from_window() fails on Pi (empty pygame.error); direct ctor works.
                renderer = sdl2_video.Renderer(window, accelerated=-1)
            except Exception as exc:
                print(f"[display] multi_window init failed on {binding.layout.display_id}: {exc!r}")
                traceback.print_exc()
                self._window_bindings.clear()
                self._window_ids.clear()
                self._interaction_window_ids.clear()
                self._focused_window_id = None
                return False
            wb = WindowBinding(
                layout=layout,
                binding=binding,
                map_overlay=self._map_overlays[layout.display_id],
                window=window,
                renderer=renderer,
            )
            self._window_bindings[layout.display_id] = wb
            self._window_ids.add(int(window.id))
            if layout.display_id == self._menu_display_id:
                self._interaction_window_ids.add(int(window.id))
        self._focused_window_id = next(iter(self._window_ids), None)
        return bool(self._window_bindings)

    def _apply_display_override(self, display: dict[str, Any]) -> dict[str, Any]:
        overrides = self._theme_data.get("display_overrides", {})
        if not isinstance(overrides, dict):
            return display
        merged = dict(display)
        global_override = overrides.get("_all", {})
        if isinstance(global_override, dict):
            merged.update(global_override)
        by_id = overrides.get(display["id"], {})
        if isinstance(by_id, dict):
            merged.update(by_id)
        return merged

    def _build_single_screen_virtual_displays(self, base_display: dict[str, Any]) -> list[dict[str, Any]]:
        # Preferred v2 layout path: theme-declared round gauge packs.
        packs = self._theme_data.get("gauge_packs", [])
        if isinstance(packs, list):
            parsed_packs: list[dict[str, Any]] = []
            for idx, entry in enumerate(packs):
                if not isinstance(entry, dict):
                    continue
                display_id = str(entry.get("id", f"gauge_pack_{idx}")).strip()
                if not display_id:
                    continue
                gauge_id = str(entry.get("gauge_id", "")).strip()
                weight_raw = entry.get("weight", 1.0)
                try:
                    weight = max(0.1, float(weight_raw))
                except (TypeError, ValueError):
                    weight = 1.0
                parsed_packs.append({"id": display_id, "gauge_id": gauge_id, "weight": weight})
            if parsed_packs:
                total_weight = sum(pack["weight"] for pack in parsed_packs)
                width = int(base_display.get("width", 800))
                height = int(base_display.get("height", 480))
                x_cursor = 0
                virtual_displays: list[dict[str, Any]] = []
                for idx, pack in enumerate(parsed_packs):
                    remaining_w = max(1, width - x_cursor)
                    if idx == len(parsed_packs) - 1:
                        panel_w = remaining_w
                    else:
                        panel_w = max(1, int(round(width * (pack["weight"] / max(0.001, total_weight)))))
                        panel_w = min(panel_w, remaining_w)
                    diameter = max(1, min(panel_w, height))
                    x_pack = int(x_cursor + (panel_w - diameter) // 2)
                    y_pack = int((height - diameter) // 2)
                    virtual = dict(base_display)
                    virtual["id"] = pack["id"]
                    virtual["shape"] = "round"
                    virtual["position"] = [x_pack, y_pack]
                    virtual["width"] = int(diameter)
                    virtual["height"] = int(diameter)
                    if pack["gauge_id"]:
                        virtual["gauge_id"] = pack["gauge_id"]
                    virtual["enabled"] = True
                    virtual_displays.append(virtual)
                    x_cursor += panel_w
                return virtual_displays

        # Allow themes to declare explicit virtual panels for a single physical screen.
        declared = self._theme_data.get("single_screen_displays", [])
        if isinstance(declared, list):
            parsed: list[dict[str, Any]] = []
            for entry in declared:
                if not isinstance(entry, dict):
                    continue
                display_id = entry.get("id")
                if not isinstance(display_id, str) or not display_id.strip():
                    continue
                virtual = dict(base_display)
                virtual["id"] = display_id.strip()
                for key in ("shape", "width", "height", "position", "gauge_id", "monitor_index", "windowed", "fullscreen"):
                    if key in entry:
                        virtual[key] = entry[key]
                virtual["enabled"] = True
                parsed.append(virtual)
            if len(parsed) >= 2:
                return parsed

        system_cfg = self._config.get("system", {})
        auto_compose = bool(system_cfg.get("single_screen_auto_compose", False))
        if not auto_compose:
            return []

        # Backward-compatible auto composition: when a theme already defines
        # canonical panel overrides, synthesize those panels on single-screen setups.
        overrides = self._theme_data.get("display_overrides", {})
        if not isinstance(overrides, dict):
            overrides = {}
        canonical_ids = [display_id for display_id in ("gauge_left", "gauge_center", "gauge_right") if isinstance(overrides.get(display_id), dict)]
        if len(canonical_ids) < 2:
            return []
        use_canonical_ids = canonical_ids

        gauge_cfg = self._config.get("gauges", {})
        gauge_ids = [key for key in gauge_cfg.keys() if isinstance(key, str)]
        preferred_order = ["speedometer", "tachometer", "tsps"]
        selected_gauges: list[str] = []
        for gauge_id in preferred_order + gauge_ids:
            if gauge_id in gauge_ids and gauge_id not in selected_gauges:
                selected_gauges.append(gauge_id)
            if len(selected_gauges) >= len(use_canonical_ids):
                break
        while len(selected_gauges) < len(use_canonical_ids):
            selected_gauges.append(selected_gauges[-1] if selected_gauges else "")

        width = int(base_display.get("width", 800))
        height = int(base_display.get("height", 480))
        left_width = min(height, max(int(width * 0.56), width // 2))
        right_width = max(0, width - left_width)
        top_height = max(1, height // 2)
        bottom_height = max(1, height - top_height)

        if right_width < max(120, int(width * 0.2)):
            col_width = max(1, width // 3)
            frames = [
                (0, 0, col_width, height),
                (col_width, 0, col_width, height),
                (col_width * 2, 0, max(1, width - (col_width * 2)), height),
            ]
        else:
            frames = [
                (0, 0, left_width, height),
                (left_width, 0, right_width, top_height),
                (left_width, top_height, right_width, bottom_height),
            ]

        virtual_displays: list[dict[str, Any]] = []
        for idx, display_id in enumerate(use_canonical_ids):
            x, y, panel_w, panel_h = frames[min(idx, len(frames) - 1)]
            virtual = dict(base_display)
            virtual["id"] = display_id
            virtual["position"] = [int(x), int(y)]
            virtual["width"] = int(panel_w)
            virtual["height"] = int(panel_h)
            if idx < len(selected_gauges) and selected_gauges[idx]:
                virtual["gauge_id"] = selected_gauges[idx]
            virtual["enabled"] = True
            virtual_displays.append(virtual)
        return virtual_displays

    def _build_layouts(self) -> list[DisplayLayout]:
        layouts: list[DisplayLayout] = []
        enabled_displays = [display for display in self._config["displays"] if bool(display.get("enabled", False))]
        source_displays = enabled_displays
        if len(enabled_displays) == 1:
            composed = self._build_single_screen_virtual_displays(enabled_displays[0])
            if composed:
                source_displays = composed

        for display in source_displays:
            resolved = self._apply_display_override(display)
            if not resolved["enabled"]:
                continue
            vrot = self._optional_view_rotation_deg(resolved)
            layouts.append(
                DisplayLayout(
                    display_id=resolved["id"],
                    role=resolved["role"],
                    shape="round" if str(resolved.get("shape", "round")).strip().lower() == "rectangle" else resolved["shape"],
                    width=resolved["width"],
                    height=resolved["height"],
                    x=resolved["position"][0],
                    y=resolved["position"][1],
                    fps=resolved["fps"],
                    gauge_id=resolved.get("gauge_id"),
                    monitor_index=resolved.get("monitor_index"),
                    windowed=bool(resolved.get("windowed", True)),
                    fullscreen=bool(resolved.get("fullscreen", False)),
                    view_rotation_deg=vrot,
                )
            )
        if self._config["system"].get("left_to_right_preview", False):
            layouts = self._reflow_left_to_right(layouts)
        return layouts

    def _x11_screen_dimensions_px(self) -> tuple[int, int] | None:
        """Physical X11 Screen 0 size — used for canvas_triple_hdmi_split and canvas_scale.

        Prefer xdpyinfo when x11-utils is installed; many Pi images omit it, so we fall back to
        xrandr (parse ``current WxH``), then pygame desktop sizes after display.init().
        Optional: ELC_SCREEN_WIDTH / ELC_SCREEN_HEIGHT (integers) to force dimensions on the vehicle.
        """
        display = str(os.environ.get("DISPLAY", "")).strip()
        if not display:
            return None
        ew = str(os.environ.get("ELC_SCREEN_WIDTH", "")).strip()
        eh = str(os.environ.get("ELC_SCREEN_HEIGHT", "")).strip()
        if ew.isdigit() and eh.isdigit():
            return int(ew), int(eh)
        env = {**os.environ}
        # 1) xdpyinfo (x11-utils)
        try:
            proc = subprocess.run(
                ["xdpyinfo", "-display", display],
                capture_output=True,
                text=True,
                timeout=4,
                env=env,
            )
            if proc.returncode == 0:
                m = re.search(r"dimensions:\s*(\d+)x(\d+)", proc.stdout or "")
                if m:
                    return int(m.group(1)), int(m.group(2))
        except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
            pass
        except Exception:
            pass
        # 2) xrandr — usually available wherever Xorg runs
        try:
            proc = subprocess.run(
                ["xrandr", "-display", display],
                capture_output=True,
                text=True,
                timeout=4,
                env=env,
            )
            if proc.returncode == 0:
                m = re.search(r"current\s+(\d+)\s+x\s+(\d+)", proc.stdout or "")
                if m:
                    return int(m.group(1)), int(m.group(2))
        except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
            pass
        except Exception:
            pass
        # 3) pygame (DisplayManager is created after pygame.display.init() in app.run)
        try:
            if pygame.display.get_init():
                sizes = pygame.display.get_desktop_sizes()
                if sizes and len(sizes[0]) >= 2:
                    return int(sizes[0][0]), int(sizes[0][1])
        except Exception:
            pass
        return None

    def _use_triple_hdmi_split(self) -> bool:
        """2160×1080 + HDMI splitter (1080|1080) + overlapping center round: uniform canvas_scale breaks column boundaries."""
        system_cfg = self._config.get("system", {})
        if not bool(system_cfg.get("canvas_triple_hdmi_split", False)):
            return False
        if len(self._layouts) != 3:
            return False
        ids = {layout.display_id for layout in self._layouts}
        if not {"round_left", "round_four_inch", "round_right"} <= ids:
            return False
        if str(os.environ.get("HEADLESS", "")).strip() == "1":
            return False
        env_mode = str(os.environ.get("DISPLAY_RENDER_MODE", "")).strip().lower()
        cfg_mode = str(system_cfg.get("display_render_mode", "single_window")).strip().lower()
        if (env_mode or cfg_mode or "single_window") == "multi_window":
            return False
        pair = self._x11_screen_dimensions_px()
        if pair is None:
            return False
        dw, dh = pair
        if dh != 1080:
            return False
        if abs(dw - 2160) > 8:
            return False
        return True

    def _apply_triple_hdmi_2160_split_layout(self) -> None:
        pair = self._x11_screen_dimensions_px()
        if pair is None:
            return
        dw, _dh = pair
        ref = int(self._config.get("system", {}).get("triple_hdmi_canvas_ref_width", 2880))
        if ref < 1:
            ref = 2880
        center_x = (1080 * dw) // ref
        by_id = {lay.display_id: lay for lay in self._layouts}
        left = by_id["round_left"]
        center = by_id["round_four_inch"]
        right = by_id["round_right"]
        # Non-overlapping strips that sum to dw: splitter still shows 0–1079 | 1080–2159; we avoid
        # drawing TSPS in 1080–center_x+719 so the tach layer does not paint over the right gauge.
        left_w = center_x
        right_x = center_x + 720
        right_w = dw - right_x
        if left_w < 1 or right_w < 1:
            return
        self._layouts = [
            replace(left, x=0, width=left_w),
            replace(center, x=center_x, width=720),
            replace(right, x=right_x, width=right_w),
        ]
        print(
            f"[display] canvas_triple_hdmi_split: strips 0–{left_w - 1} | {center_x}–{center_x + 719} | "
            f"{right_x}–{dw - 1} (TSPS no longer under tach); xrandr center @x={center_x}",
            flush=True,
        )

    def _apply_responsive_canvas_layout(self) -> None:
        want_triple = bool(self._config.get("system", {}).get("canvas_triple_hdmi_split", False))
        if self._use_triple_hdmi_split():
            self._apply_triple_hdmi_2160_split_layout()
            return
        if want_triple:
            pair = self._x11_screen_dimensions_px()
            print(
                f"[display] canvas_triple_hdmi_split is true but layout not applied "
                f"(screen={pair}; need ~2160×1080). Using canvas_scale_to_desktop_width instead. "
                f"Set ELC_SCREEN_WIDTH=2160 ELC_SCREEN_HEIGHT=1080 or install x11-utils (xdpyinfo).",
                flush=True,
            )
        self._scale_layouts_to_desktop_width_if_needed()

    def _scale_layouts_to_desktop_width_if_needed(self) -> None:
        """When the X11 desktop is narrower than the YAML canvas (e.g. 2160 vs 2880) but the same
        height, scale each panel's x/width proportionally so we draw 1:1 into the window instead of
        smoothscaling the full off-screen canvas — avoids horizontal squish and tach bleeding onto 5″ panels."""
        if not self._layouts:
            return
        if str(os.environ.get("HEADLESS", "")).strip() == "1":
            return
        env_mode = str(os.environ.get("DISPLAY_RENDER_MODE", "")).strip().lower()
        cfg_mode = str(self._config.get("system", {}).get("display_render_mode", "single_window")).strip().lower()
        if (env_mode or cfg_mode or "single_window") == "multi_window":
            return
        system_cfg = self._config.get("system", {})
        if not bool(system_cfg.get("canvas_scale_to_desktop_width", True)):
            return
        pair = self._x11_screen_dimensions_px()
        if pair is not None:
            dw, dh = pair
        else:
            try:
                sizes = pygame.display.get_desktop_sizes()
                if not sizes or len(sizes[0]) < 2:
                    return
                dw, dh = int(sizes[0][0]), int(sizes[0][1])
            except Exception:
                return
        if dw < 320 or dh < 240:
            return
        canvas_w, canvas_h = self._canvas_size_from_layouts()
        if canvas_w <= 0 or canvas_h <= 0:
            return
        if dh != canvas_h:
            return
        if dw >= canvas_w:
            return
        scale = dw / float(canvas_w)
        new_layouts: list[DisplayLayout] = []
        for layout in self._layouts:
            nx = int(round(layout.x * scale))
            nw = max(1, int(round(layout.width * scale)))
            new_layouts.append(
                DisplayLayout(
                    display_id=layout.display_id,
                    role=layout.role,
                    shape=layout.shape,
                    width=nw,
                    height=layout.height,
                    x=nx,
                    y=layout.y,
                    fps=layout.fps,
                    gauge_id=layout.gauge_id,
                    monitor_index=layout.monitor_index,
                    windowed=layout.windowed,
                    fullscreen=layout.fullscreen,
                    view_rotation_deg=layout.view_rotation_deg,
                )
            )
        if not new_layouts:
            return
        last = new_layouts[-1]
        gap = dw - (last.x + last.width)
        if gap != 0 and last.width + gap > 0:
            new_layouts[-1] = DisplayLayout(
                display_id=last.display_id,
                role=last.role,
                shape=last.shape,
                width=last.width + gap,
                height=last.height,
                x=last.x,
                y=last.y,
                fps=last.fps,
                gauge_id=last.gauge_id,
                monitor_index=last.monitor_index,
                windowed=last.windowed,
                fullscreen=last.fullscreen,
                view_rotation_deg=last.view_rotation_deg,
            )
        self._layouts = new_layouts
        print(
            f"[display] canvas_scale_to_desktop_width: {canvas_w}x{canvas_h} → {dw}x{dh} "
            f"(scale={scale:.4f}); 1:1 blit (no horizontal composite squash)",
            flush=True,
        )

    def _reflow_left_to_right(self, layouts: list[DisplayLayout]) -> list[DisplayLayout]:
        spacing = int(self._config["system"].get("left_to_right_spacing_px", 16))
        next_x = 0
        reflowed: list[DisplayLayout] = []
        for layout in layouts:
            reflowed.append(
                DisplayLayout(
                    display_id=layout.display_id,
                    role=layout.role,
                    shape=layout.shape,
                    width=layout.width,
                    height=layout.height,
                    x=next_x,
                    y=0,
                    fps=layout.fps,
                    gauge_id=layout.gauge_id,
                    monitor_index=layout.monitor_index,
                    windowed=layout.windowed,
                    fullscreen=layout.fullscreen,
                    view_rotation_deg=layout.view_rotation_deg,
                )
            )
            next_x += layout.width + spacing
        return reflowed

    def _build_bindings(self) -> list[ScreenBinding]:
        bindings: list[ScreenBinding] = []
        for layout in self._layouts:
            bindings.append(ScreenBinding(layout=layout, renderer=None))
        return bindings

    def _ensure_renderer(self, binding: ScreenBinding) -> GaugeScreen:
        if binding.renderer is None:
            binding.renderer = GaugeScreen(layout=binding.layout, config=self._config)
        return binding.renderer

    def _resolve_menu_display_id(self) -> str:
        configured = str(self._config.get("system", {}).get("menu_display_id", "gauge_right"))
        known_ids = {layout.display_id for layout in self._layouts}
        if configured in known_ids:
            return configured
        if self._layouts:
            return max(self._layouts, key=lambda layout: layout.width * layout.height).display_id
        return configured

    def _resolve_menu_layout(self) -> DisplayLayout:
        for layout in self._layouts:
            if layout.display_id == self._menu_display_id:
                return layout
        if self._layouts:
            return self._layouts[0]
        return DisplayLayout(
            display_id=self._menu_display_id,
            role="gauge",
            shape="round",
            width=640,
            height=480,
            x=0,
            y=0,
            fps=30,
            gauge_id=None,
            view_rotation_deg=None,
        )

    def _resolve_menu_render_on_oled(self) -> bool:
        return bool(self._config.get("system", {}).get("menu_render_on_oled", False))

    def _resolve_alert_display_id(self) -> str:
        configured = str(self._config.get("system", {}).get("alert_display_id", "gauge_center"))
        known_ids = {layout.display_id for layout in self._layouts}
        if configured in known_ids:
            return configured
        if self._layouts:
            return max(self._layouts, key=lambda layout: layout.width * layout.height).display_id
        return configured

    def _resolve_alert_layout(self) -> DisplayLayout:
        for layout in self._layouts:
            if layout.display_id == self._alert_display_id:
                return layout
        if self._layouts:
            return self._layouts[0]
        return DisplayLayout(
            display_id=self._alert_display_id,
            role="gauge",
            shape="round",
            width=640,
            height=480,
            x=0,
            y=0,
            fps=30,
            gauge_id=None,
            view_rotation_deg=None,
        )

    def _build_map_overlays(self) -> dict[str, MapScreen]:
        overlays: dict[str, MapScreen] = {}
        for layout in self._layouts:
            overlays[layout.display_id] = MapScreen(layout=layout, config=self._config)
        return overlays

    def refresh_theme(self) -> bool:
        old_signature = self._layout_signature(self._layouts)
        self._theme_data = self._load_theme_data()
        self._layouts = self._build_layouts()
        self._apply_responsive_canvas_layout()
        self._menu_display_id = self._resolve_menu_display_id()
        self._menu_layout = self._resolve_menu_layout()
        self._menu_render_on_oled = self._resolve_menu_render_on_oled()
        self._alert_display_id = self._resolve_alert_display_id()
        self._alert_layout = self._resolve_alert_layout()
        self._menu_overlay = MenuOverlayController(
            layout=self._menu_layout,
            config=self._config,
            config_path=self._config_path,
            command_registry=self._command_registry,
            on_runtime_refresh=self._on_runtime_refresh,
        )
        self._warning_overlay = WarningOverlayScreen(layout=self._alert_layout, config=self._config)
        self._bindings = self._build_bindings()
        self._map_overlays = self._build_map_overlays()

        new_signature = self._layout_signature(self._layouts)
        if new_signature != old_signature:
            return False

        if self._render_mode == "multi_window":
            by_id = {binding.layout.display_id: binding for binding in self._bindings}
            for display_id, window_binding in list(self._window_bindings.items()):
                binding = by_id.get(display_id)
                overlay = self._map_overlays.get(display_id)
                if binding is None or overlay is None:
                    return False
                window_binding.binding = binding
                window_binding.map_overlay = overlay
            self._interaction_window_ids.clear()
            for display_id, wb in self._window_bindings.items():
                if display_id == self._menu_display_id:
                    self._interaction_window_ids.add(int(wb.window.id))
        return True

    def _map_overlay_visible(self, layout: DisplayLayout) -> bool:
        system_cfg = self._config.get("system", {})
        visible = bool(system_cfg.get("map_view_visible", False))
        if not visible:
            return False
        target_display = str(system_cfg.get("map_display_id", "gauge_right"))
        known_ids = {entry.display_id for entry in self._layouts}
        if target_display not in known_ids and self._layouts:
            target_display = self._menu_display_id
        return layout.display_id == target_display

    def _menu_gauge_overlay_visible(self, layout: DisplayLayout) -> bool:
        return (
            self._menu_overlay.visible
            and layout.display_id == self._menu_display_id
            and not self._menu_render_on_oled
        )

    def _warning_overlay_visible(self, layout: DisplayLayout, values: dict[str, float]) -> bool:
        if layout.display_id != self._alert_display_id:
            return False
        active_count = int(values.get("arduino.fault_latched_count", values.get("arduino.fault_active_count", 0.0)))
        return active_count > 0

    def _canvas_size_from_layouts(self) -> tuple[int, int]:
        if not self._layouts:
            return (640, 480)
        max_x = max(layout.x + layout.width for layout in self._layouts)
        max_y = max(layout.y + layout.height for layout in self._layouts)
        return (max_x, max_y)

    def _render_single_window(self, values: dict[str, float], status: dict[str, str]) -> None:
        if self._root_surface is None:
            return
        target = self._composite_surface if self._composite_surface is not None else self._root_surface
        target.fill((12, 12, 12))
        for binding in self._bindings:
            renderer = self._ensure_renderer(binding)
            child = self._compose_child_surface(
                layout=binding.layout,
                renderer=renderer,
                map_overlay=self._map_overlays[binding.layout.display_id],
                values=values,
                status=status,
            )
            child = self._flip_surface_for_layout(binding.layout, child)
            target.blit(child, (binding.layout.x, binding.layout.y))
        if self._composite_surface is not None:
            scaled = pygame.transform.smoothscale(self._composite_surface, self._physical_size)
            self._root_surface.blit(scaled, (0, 0))
        if str(os.environ.get("HEADLESS", "")).strip() != "1":
            pygame.display.flip()

    def _render_multi_window(self, values: dict[str, float], status: dict[str, str]) -> None:
        if self._sdl2_texture is None:
            return
        for binding in self._window_bindings.values():
            renderer = self._ensure_renderer(binding.binding)
            child = self._compose_child_surface(
                layout=binding.layout,
                renderer=renderer,
                map_overlay=binding.map_overlay,
                values=values,
                status=status,
            )
            child = self._flip_surface_for_layout(binding.layout, child)
            texture = self._sdl2_texture.from_surface(binding.renderer, child)
            binding.renderer.draw_color = (12, 12, 12, 255)
            binding.renderer.clear()
            binding.renderer.blit(texture)
            binding.renderer.present()

    def _event_window_id(self, event: pygame.event.Event) -> int | None:
        raw = getattr(event, "window", None)
        if raw is None:
            raw = getattr(event, "windowID", None)
        if raw is None:
            return None
        if hasattr(raw, "id"):
            raw = getattr(raw, "id")
        try:
            return int(raw)
        except (TypeError, ValueError):
            return None

    def handle_event(self, event: pygame.event.Event) -> bool:
        if event.type == pygame.QUIT:
            return True
        if self._render_mode != "multi_window":
            return False
        if event.type == pygame.WINDOWFOCUSGAINED:
            win_id = self._event_window_id(event)
            if win_id is not None and win_id in self._window_ids:
                self._focused_window_id = win_id
        elif event.type == pygame.WINDOWFOCUSLOST:
            win_id = self._event_window_id(event)
            if win_id is not None and win_id == self._focused_window_id:
                self._focused_window_id = None
        elif event.type == pygame.WINDOWCLOSE:
            win_id = self._event_window_id(event)
            if win_id is not None:
                self._window_ids.discard(win_id)
                self._interaction_window_ids.discard(win_id)
                if win_id == self._focused_window_id:
                    self._focused_window_id = None
            if not self._window_ids:
                return True
        return False

    def keyboard_input_active(self) -> bool:
        system_cfg = self._config.get("system", {})
        if str(system_cfg.get("mode", "")).strip().lower() == "simulation":
            return True
        if self._menu_render_on_oled and self._menu_overlay.visible:
            return True
        if not bool(system_cfg.get("keyboard_focus_control_only", False)):
            return True
        if self._render_mode != "multi_window":
            return True
        if not self._interaction_window_ids:
            return True
        return self._focused_window_id in self._interaction_window_ids

    def render(self, values: dict[str, float], status: dict[str, str]) -> None:
        self._menu_overlay.process_input(values=values, status=status)
        if self._render_mode == "multi_window":
            self._render_multi_window(values, status)
            return
        self._render_single_window(values, status)

    def shutdown(self) -> None:
        pass

    def close_menu_overlay(self) -> None:
        self._menu_overlay.close_overlay()

    def menu_overlay_visible(self) -> bool:
        return bool(self._menu_overlay.visible)

    def menu_render_on_oled(self) -> bool:
        return self._menu_render_on_oled

    def oled_menu_nano_payload(self, status: dict[str, str]) -> dict[str, str | int] | None:
        return self._menu_overlay.oled_menu_nano_payload(status)

    def menu_open_or_back(self) -> str:
        return self._menu_overlay.open_or_back()
