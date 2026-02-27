from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
from typing import Any
from typing import Callable

import pygame

from dashboard.screens.control_screen import ControlScreen
from dashboard.screens.gauge_screen import GaugeScreen
from dashboard.screens.layout import DisplayLayout
from dashboard.screens.map_screen import MapScreen


@dataclass
class ScreenBinding:
    layout: DisplayLayout
    renderer: GaugeScreen | ControlScreen


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
        self._bindings = self._build_bindings()
        self._map_overlays = self._build_map_overlays()
        self._render_mode = self._resolve_render_mode()
        self._window_bindings: dict[str, WindowBinding] = {}
        self._window_ids: set[int] = set()
        self._control_window_ids: set[int] = set()
        self._focused_window_id: int | None = None
        self._root_surface: pygame.Surface | None = None
        self._sdl2_video: Any | None = None
        self._sdl2_texture: Any | None = None

        if self._render_mode == "multi_window" and not self._init_multi_window_mode():
            self._render_mode = "single_window"
        if self._render_mode == "single_window":
            self._init_single_window_mode()

    def _load_theme_data(self) -> dict[str, Any]:
        theme_root = Path(self._config["themes"]["theme_root"])
        theme_name = self._config["themes"]["active"]
        theme_path = theme_root / theme_name / "theme.json"
        if not theme_path.exists():
            return {}
        try:
            return json.loads(theme_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return {}

    def _resolve_render_mode(self) -> str:
        system_cfg = self._config.get("system", {})
        env_mode = str(os.environ.get("DISPLAY_RENDER_MODE", "")).strip().lower()
        cfg_mode = str(system_cfg.get("display_render_mode", "single_window")).strip().lower()
        mode = env_mode or cfg_mode or "single_window"
        if mode not in {"single_window", "multi_window"}:
            return "single_window"
        return mode

    def _init_single_window_mode(self) -> None:
        self._root_surface = pygame.display.set_mode(self._window_size())
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
                self._control_window_ids.clear()
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
            if layout.role == "control":
                self._control_window_ids.add(int(window.id))
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

    def _build_layouts(self) -> list[DisplayLayout]:
        layouts: list[DisplayLayout] = []
        for display in self._config["displays"]:
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
            if layout.role == "control":
                renderer = ControlScreen(
                    layout=layout,
                    config=self._config,
                    config_path=self._config_path,
                    command_registry=self._command_registry,
                    on_runtime_refresh=self._on_runtime_refresh,
                )
            else:
                renderer = GaugeScreen(layout=layout, config=self._config)
            bindings.append(ScreenBinding(layout=layout, renderer=renderer))
        return bindings

    def _build_map_overlays(self) -> dict[str, MapScreen]:
        overlays: dict[str, MapScreen] = {}
        for layout in self._layouts:
            overlays[layout.display_id] = MapScreen(layout=layout, config=self._config)
        return overlays

    def _map_overlay_visible(self, layout: DisplayLayout) -> bool:
        system_cfg = self._config.get("system", {})
        visible = bool(system_cfg.get("map_view_visible", False))
        if not visible:
            return False
        target_display = str(system_cfg.get("map_display_id", "gauge_right"))
        return layout.display_id == target_display

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
            child = pygame.Surface(binding.layout.size, pygame.SRCALPHA)
            binding.renderer.render(child, values=values, status=status)
            if self._map_overlay_visible(binding.layout):
                overlay = pygame.Surface(binding.layout.size, pygame.SRCALPHA)
                self._map_overlays[binding.layout.display_id].render(overlay, values=values, status=status)
                child.blit(overlay, (0, 0))
            self._root_surface.blit(child, (binding.layout.x, binding.layout.y))
        pygame.display.flip()

    def _render_multi_window(self, values: dict[str, float], status: dict[str, str]) -> None:
        if self._sdl2_texture is None:
            return
        for binding in self._window_bindings.values():
            child = pygame.Surface(binding.layout.size, pygame.SRCALPHA)
            binding.binding.renderer.render(child, values=values, status=status)
            if self._map_overlay_visible(binding.layout):
                overlay = pygame.Surface(binding.layout.size, pygame.SRCALPHA)
                binding.map_overlay.render(overlay, values=values, status=status)
                child.blit(overlay, (0, 0))
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
                self._control_window_ids.discard(win_id)
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
        if not self._control_window_ids:
            return True
        return self._focused_window_id in self._control_window_ids

    def render(self, values: dict[str, float], status: dict[str, str]) -> None:
        if self._render_mode == "multi_window":
            self._render_multi_window(values, status)
            return
        self._render_single_window(values, status)
