from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any
from typing import Callable

import pygame

from dashboard.screens.control_screen import ControlScreen
from dashboard.screens.gauge_screen import GaugeScreen
from dashboard.screens.layout import DisplayLayout


@dataclass
class ScreenBinding:
    layout: DisplayLayout
    renderer: GaugeScreen | ControlScreen


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
        self._root_surface = pygame.display.set_mode(self._window_size())
        pygame.display.set_caption(config["system"]["window_title"])

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

    def _window_size(self) -> tuple[int, int]:
        max_x = max(layout.x + layout.width for layout in self._layouts)
        max_y = max(layout.y + layout.height for layout in self._layouts)
        return (max_x, max_y)

    def render(self, values: dict[str, float], status: dict[str, str]) -> None:
        self._root_surface.fill((12, 12, 12))
        for binding in self._bindings:
            child = pygame.Surface(binding.layout.size, pygame.SRCALPHA)
            binding.renderer.render(child, values=values, status=status)
            self._root_surface.blit(child, (binding.layout.x, binding.layout.y))
        pygame.display.flip()
