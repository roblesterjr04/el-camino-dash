from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
from typing import Any
from typing import Callable

import pygame

from dashboard.screens.gauge_screen import GaugeScreen
from dashboard.screens.layout import DisplayLayout
from dashboard.screens.map_screen import MapScreen
from dashboard.screens.menu_overlay import MenuOverlayController
from dashboard.screens.warning_overlay import WarningOverlayScreen
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
        self._theme_data = self._load_theme_data()
        self._layouts = self._build_layouts()
        self._menu_display_id = self._resolve_menu_display_id()
        self._menu_layout = self._resolve_menu_layout()
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
        self._spi_display_manager: Any | None = None
        self._startup_placeholder_drawn = False

        if self._render_mode == "multi_window" and not self._init_multi_window_mode():
            self._render_mode = "single_window"
        if self._render_mode == "spi":
            self._init_spi_mode()
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
                texture = self._sdl2_texture.from_surface(binding.renderer, scaled)
                binding.renderer.draw_color = (0, 0, 0, 255)
                binding.renderer.clear()
                binding.renderer.blit(texture)
                binding.renderer.present()
            self._startup_placeholder_drawn = True
            elapsed = max(0.0, (pygame.time.get_ticks() - started_ms) / 1000.0)
            print(f"[startup] placeholder render done after {elapsed:.3f}s", flush=True)
            return

        if self._render_mode == "spi" and self._spi_display_manager is not None:
            for idx, binding in enumerate(self._bindings):
                child = pygame.transform.smoothscale(image, binding.layout.size)
                self._spi_display_manager.update(idx, child)
                if self._root_surface is not None:
                    self._root_surface.blit(child, (binding.layout.x, binding.layout.y))
            if self._root_surface is not None:
                pygame.display.flip()
            self._startup_placeholder_drawn = True
            elapsed = max(0.0, (pygame.time.get_ticks() - started_ms) / 1000.0)
            print(f"[startup] placeholder render done after {elapsed:.3f}s", flush=True)
            return

        if self._root_surface is not None:
            scaled = pygame.transform.smoothscale(image, self._window_size())
            self._root_surface.blit(scaled, (0, 0))
            if str(os.environ.get("HEADLESS", "")).strip() != "1":
                pygame.display.flip()
            self._startup_placeholder_drawn = True
            elapsed = max(0.0, (pygame.time.get_ticks() - started_ms) / 1000.0)
            print(f"[startup] placeholder render done after {elapsed:.3f}s", flush=True)

    def _load_theme_data(self) -> dict[str, Any]:
        theme_root = Path(self._config["themes"]["theme_root"])
        theme_name = self._config["themes"]["active"]
        return load_theme_data(theme_root=theme_root, theme_name=theme_name)

    def _resolve_render_mode(self) -> str:
        system_cfg = self._config.get("system", {})
        env_mode = str(os.environ.get("DISPLAY_RENDER_MODE", "")).strip().lower()
        cfg_mode = str(system_cfg.get("display_render_mode", "single_window")).strip().lower()
        mode = env_mode or cfg_mode or "single_window"
        if mode not in {"single_window", "multi_window", "spi"}:
            return "single_window"
        return mode

    def _resolve_spi_displays(self) -> list[dict[str, Any]]:
        system_cfg = self._config.get("system", {})
        raw = system_cfg.get("spi_displays", [])
        if not isinstance(raw, list):
            return []
        resolved: list[dict[str, Any]] = []
        for idx, layout in enumerate(self._layouts):
            if idx >= len(raw):
                break
            spec = raw[idx]
            if not isinstance(spec, dict):
                continue
            dc_pin = spec.get("dc_pin", spec.get("dc"))
            rst_pin = spec.get("rst_pin", spec.get("rst"))
            if dc_pin is None or rst_pin is None:
                continue
            resolved.append(
                {
                    "label": spec.get("label", layout.display_id),
                    "shape": layout.shape,
                    "width": layout.width,
                    "height": layout.height,
                    "spi_bus": int(spec.get("spi_bus", 1)),
                    "spi_device": int(spec.get("spi_device", idx)),
                    "dc_pin": int(dc_pin),
                    "rst_pin": int(rst_pin),
                    "rotation": int(spec.get("rotation", 0)),
                    "madctl": spec.get("madctl"),
                    "colmod": int(spec.get("colmod", 0x55)),
                    "speed_hz": int(spec.get("speed_hz", 8_000_000)),
                }
            )
        return resolved

    def _init_spi_mode(self) -> None:
        from dashboard.display.spi_display_manager import SPIDisplayManager

        displays = self._resolve_spi_displays()
        if not displays:
            print("[display] spi mode requested, but no valid system.spi_displays entries found")
            return
        if str(os.environ.get("HEADLESS", "")).strip() != "1":
            self._root_surface = pygame.display.set_mode(self._window_size())
            pygame.display.set_caption(self._config["system"]["window_title"])
        try:
            self._spi_display_manager = SPIDisplayManager(displays=displays)
        except Exception as exc:
            print(f"[display] failed to initialize spi mode: {exc}")
            self._spi_display_manager = None

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
        if self._menu_overlay_visible(layout):
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

    def _init_single_window_mode(self) -> None:
        if str(os.environ.get("HEADLESS", "")).strip() == "1":
            self._root_surface = pygame.Surface(self._window_size(), pygame.SRCALPHA)
            return
        flags = 0
        # In single-window hardware mode, respect display fullscreen intent to
        # avoid a decorated desktop window/titlebar on the dash screen.
        if len(self._layouts) == 1 and self._layouts[0].fullscreen:
            # SDL fullscreen mode switches can stall HDMI bring-up on some Pi/X11
            # paths. Use borderless full-window mode by default for faster first paint.
            use_legacy_fullscreen = bool(self._config.get("system", {}).get("legacy_fullscreen_mode", False))
            if use_legacy_fullscreen:
                flags |= pygame.FULLSCREEN
            else:
                flags |= pygame.NOFRAME
                os.environ.setdefault("SDL_VIDEO_WINDOW_POS", "0,0")
        started = pygame.time.get_ticks()
        print(f"[startup] pygame.display.set_mode begin flags={flags}", flush=True)
        self._root_surface = pygame.display.set_mode(self._window_size(), flags)
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
        for binding in self._bindings:
            try:
                layout = binding.layout
                kwargs: dict[str, Any] = {"fullscreen": False}
                window = sdl2_video.Window(
                    title=f"{title_root} [{layout.display_id}]",
                    size=layout.size,
                    position=(layout.x, layout.y),
                    **kwargs,
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
                renderer = sdl2_video.Renderer.from_window(window)
            except Exception as exc:
                print(f"[display] multi_window init failed on {binding.layout.display_id}: {exc}")
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
            layouts.append(
                DisplayLayout(
                    display_id=resolved["id"],
                    role=resolved["role"],
                    shape=resolved["shape"],
                    width=resolved["width"],
                    height=resolved["height"],
                    x=resolved["position"][0],
                    y=resolved["position"][1],
                    fps=resolved["fps"],
                    gauge_id=resolved.get("gauge_id"),
                    monitor_index=resolved.get("monitor_index"),
                    windowed=bool(resolved.get("windowed", True)),
                    fullscreen=bool(resolved.get("fullscreen", False)),
                )
            )
        if self._config["system"].get("left_to_right_preview", False):
            layouts = self._reflow_left_to_right(layouts)
        return layouts

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
        )

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
        self._menu_display_id = self._resolve_menu_display_id()
        self._menu_layout = self._resolve_menu_layout()
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

    def _menu_overlay_visible(self, layout: DisplayLayout) -> bool:
        return self._menu_overlay.visible and layout.display_id == self._menu_display_id

    def _warning_overlay_visible(self, layout: DisplayLayout, values: dict[str, float]) -> bool:
        if layout.display_id != self._alert_display_id:
            return False
        active_count = int(values.get("arduino.fault_latched_count", values.get("arduino.fault_active_count", 0.0)))
        return active_count > 0

    def _window_size(self) -> tuple[int, int]:
        if not self._layouts:
            return (640, 480)
        max_x = max(layout.x + layout.width for layout in self._layouts)
        max_y = max(layout.y + layout.height for layout in self._layouts)
        return (max_x, max_y)

    def _render_single_window(self, values: dict[str, float], status: dict[str, str]) -> None:
        if self._root_surface is None:
            return
        self._root_surface.fill((12, 12, 12))
        for binding in self._bindings:
            renderer = self._ensure_renderer(binding)
            child = self._compose_child_surface(
                layout=binding.layout,
                renderer=renderer,
                map_overlay=self._map_overlays[binding.layout.display_id],
                values=values,
                status=status,
            )
            self._root_surface.blit(child, (binding.layout.x, binding.layout.y))
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
            texture = self._sdl2_texture.from_surface(binding.renderer, child)
            binding.renderer.draw_color = (12, 12, 12, 255)
            binding.renderer.clear()
            binding.renderer.blit(texture)
            binding.renderer.present()

    def _render_spi(self, values: dict[str, float], status: dict[str, str]) -> None:
        if self._spi_display_manager is None:
            return
        if self._root_surface is not None:
            self._root_surface.fill((12, 12, 12))
        for idx, binding in enumerate(self._bindings):
            renderer = self._ensure_renderer(binding)
            child = self._compose_child_surface(
                layout=binding.layout,
                renderer=renderer,
                map_overlay=self._map_overlays[binding.layout.display_id],
                values=values,
                status=status,
            )
            self._spi_display_manager.update(idx, child)
            if self._root_surface is not None:
                self._root_surface.blit(child, (binding.layout.x, binding.layout.y))
        if self._root_surface is not None:
            pygame.display.flip()

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
        if self._render_mode == "spi":
            self._render_spi(values, status)
            return
        self._render_single_window(values, status)

    def shutdown(self) -> None:
        if self._spi_display_manager is not None:
            self._spi_display_manager.shutdown()
            self._spi_display_manager = None

    def close_menu_overlay(self) -> None:
        self._menu_overlay.close_overlay()

    def menu_open_or_back(self) -> str:
        return self._menu_overlay.open_or_back()
