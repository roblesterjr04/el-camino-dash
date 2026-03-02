from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

import yaml


CommandHandler = Callable[[dict[str, Any]], str | None]


@dataclass(frozen=True)
class MenuItem:
    item_id: str
    label: str
    item_type: str
    command: str | None = None
    target_page: str | None = None
    status_key: str | None = None
    config_path: str | None = None
    value_type: str | None = None
    min_value: float | None = None
    max_value: float | None = None
    step: float | None = None
    apply_mode: str = "immediate"
    on_change_command: str | None = None
    icon: str | None = None


@dataclass(frozen=True)
class MenuPage:
    page_id: str
    title: str
    items: list[MenuItem]


@dataclass(frozen=True)
class MenuSchema:
    home: str
    pages: dict[str, MenuPage]


class MenuSchemaError(Exception):
    """Raised when menu schema is invalid."""


@dataclass(frozen=True)
class MenuRenderRow:
    item_id: str
    label: str
    value_text: str
    selected: bool
    item_type: str
    icon: str | None = None


@dataclass(frozen=True)
class MenuRenderModel:
    page_id: str
    title: str
    rows: list[MenuRenderRow]
    footer: str
    message: str


def _get_by_path(config: dict[str, Any], path: str) -> Any:
    current: Any = config
    for part in path.split("."):
        if not isinstance(current, dict) or part not in current:
            raise KeyError(path)
        current = current[part]
    return current


def _set_by_path(config: dict[str, Any], path: str, value: Any) -> None:
    parts = path.split(".")
    current: dict[str, Any] = config
    for part in parts[:-1]:
        nested = current.get(part)
        if not isinstance(nested, dict):
            raise KeyError(path)
        current = nested
    if parts[-1] not in current:
        raise KeyError(path)
    current[parts[-1]] = value


def load_menu_schema(
    schema_path: str | Path,
    config: dict[str, Any],
    command_ids: set[str],
) -> MenuSchema:
    path = Path(schema_path)
    if not path.exists():
        raise MenuSchemaError(f"Menu schema not found: {path}")

    raw = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict) or "menu" not in raw or not isinstance(raw["menu"], dict):
        raise MenuSchemaError("menu: root mapping missing")
    menu = raw["menu"]
    if "home" not in menu or "pages" not in menu:
        raise MenuSchemaError("menu: missing required keys 'home'/'pages'")
    if not isinstance(menu["pages"], list):
        raise MenuSchemaError("menu.pages: expected list")

    pages: dict[str, MenuPage] = {}
    for page_idx, page_raw in enumerate(menu["pages"]):
        if not isinstance(page_raw, dict):
            raise MenuSchemaError(f"menu.pages[{page_idx}]: expected mapping")
        page_id = page_raw.get("id")
        title = page_raw.get("title")
        items_raw = page_raw.get("items")
        if not isinstance(page_id, str) or not isinstance(title, str) or not isinstance(items_raw, list):
            raise MenuSchemaError(f"menu.pages[{page_idx}]: missing/invalid id/title/items")
        if page_id in pages:
            raise MenuSchemaError(f"menu.pages[{page_idx}]: duplicate page id '{page_id}'")

        items: list[MenuItem] = []
        for item_idx, item_raw in enumerate(items_raw):
            if not isinstance(item_raw, dict):
                raise MenuSchemaError(f"menu.pages[{page_id}].items[{item_idx}]: expected mapping")
            item_type = item_raw.get("type")
            if item_type not in {"action", "toggle", "range", "submenu", "info"}:
                raise MenuSchemaError(f"menu.pages[{page_id}].items[{item_idx}]: invalid type '{item_type}'")
            item = MenuItem(
                item_id=str(item_raw.get("id", "")),
                label=str(item_raw.get("label", "")),
                item_type=item_type,
                command=item_raw.get("command"),
                target_page=item_raw.get("target_page"),
                status_key=item_raw.get("status_key"),
                config_path=item_raw.get("config_path"),
                value_type=item_raw.get("value_type"),
                min_value=float(item_raw["min"]) if "min" in item_raw else None,
                max_value=float(item_raw["max"]) if "max" in item_raw else None,
                step=float(item_raw["step"]) if "step" in item_raw else None,
                apply_mode=str(item_raw.get("apply_mode", "immediate")),
                on_change_command=item_raw.get("on_change_command"),
                icon=item_raw.get("icon"),
            )
            if not item.item_id or not item.label:
                raise MenuSchemaError(f"menu.pages[{page_id}].items[{item_idx}]: id/label required")
            if item.apply_mode not in {"immediate", "staged"}:
                raise MenuSchemaError(f"menu.pages[{page_id}].items[{item_idx}]: invalid apply_mode")

            if item.item_type == "action":
                if not item.command:
                    raise MenuSchemaError(f"menu.pages[{page_id}].items[{item_idx}]: action command required")
                if item.command not in command_ids:
                    raise MenuSchemaError(f"menu.pages[{page_id}].items[{item_idx}]: unknown command '{item.command}'")
            elif item.item_type == "submenu":
                if not item.target_page:
                    raise MenuSchemaError(f"menu.pages[{page_id}].items[{item_idx}]: target_page required")
            elif item.item_type == "info":
                if not item.status_key:
                    raise MenuSchemaError(f"menu.pages[{page_id}].items[{item_idx}]: status_key required")
            elif item.item_type in {"toggle", "range"}:
                if not item.config_path:
                    raise MenuSchemaError(f"menu.pages[{page_id}].items[{item_idx}]: config_path required")
                try:
                    _get_by_path(config, item.config_path)
                except KeyError as exc:
                    raise MenuSchemaError(
                        f"menu.pages[{page_id}].items[{item_idx}]: invalid config_path '{item.config_path}'"
                    ) from exc
                if item.item_type == "range":
                    if item.value_type not in {"int", "float"}:
                        raise MenuSchemaError(
                            f"menu.pages[{page_id}].items[{item_idx}]: range requires value_type int/float"
                        )
                    if item.min_value is None or item.max_value is None or item.step is None:
                        raise MenuSchemaError(
                            f"menu.pages[{page_id}].items[{item_idx}]: range requires min/max/step"
                        )
                if item.on_change_command and item.on_change_command not in command_ids:
                    raise MenuSchemaError(
                        f"menu.pages[{page_id}].items[{item_idx}]: unknown on_change_command"
                    )

            items.append(item)
        pages[page_id] = MenuPage(page_id=page_id, title=title, items=items)

    home = menu["home"]
    if home not in pages:
        raise MenuSchemaError(f"menu.home: unknown page id '{home}'")
    for page in pages.values():
        for item in page.items:
            if item.item_type == "submenu" and item.target_page not in pages:
                raise MenuSchemaError(
                    f"menu.pages[{page.page_id}].items[{item.item_id}]: unknown target '{item.target_page}'"
                )

    return MenuSchema(home=home, pages=pages)


class ControlMenuController:
    def __init__(
        self,
        schema: MenuSchema,
        config: dict[str, Any],
        config_path: Path,
        command_registry: dict[str, CommandHandler],
        on_runtime_refresh: Callable[[], None] | None = None,
    ) -> None:
        self._schema = schema
        self._config = config
        self._config_path = config_path
        self._command_registry = command_registry
        self._on_runtime_refresh = on_runtime_refresh
        self._current_page_id = schema.home
        self._cursor = 0
        self._editing_range = False
        self._last_press_count = 0
        self._staged_overrides: dict[str, Any] = {}
        self._message = ""

    @property
    def dirty(self) -> bool:
        return bool(self._staged_overrides)

    @property
    def current_page_id(self) -> str:
        return self._current_page_id

    @property
    def editing_range(self) -> bool:
        return self._editing_range

    def process_input(self, values: dict[str, float], status: dict[str, str]) -> None:
        delta = int(values.get("inputs.rotary.menu.delta", 0.0))
        if delta != 0:
            self.rotate(delta)
        press_count = int(values.get("inputs.rotary.menu.presses", 0.0))
        if press_count > self._last_press_count:
            for _ in range(press_count - self._last_press_count):
                self.press(status)
        self._last_press_count = press_count

    def rotate(self, delta: int) -> None:
        if delta == 0:
            return
        self._handle_rotate(delta)

    def press(self, status: dict[str, str]) -> None:
        self._activate_current(status)

    def sync_press_count(self, press_count: int) -> None:
        self._last_press_count = max(0, int(press_count))

    def reset_view(self) -> None:
        self._current_page_id = self._schema.home
        self._cursor = 0
        self._editing_range = False
        self._message = ""

    def _current_page(self) -> MenuPage:
        return self._schema.pages[self._current_page_id]

    def _current_item(self) -> MenuItem:
        page = self._current_page()
        if not page.items:
            raise MenuSchemaError(f"Page '{page.page_id}' has no items")
        self._cursor = max(0, min(self._cursor, len(page.items) - 1))
        return page.items[self._cursor]

    def _handle_rotate(self, delta: int) -> None:
        page = self._current_page()
        if not page.items:
            return
        if self._editing_range and self._current_item().item_type == "range":
            self._adjust_range(delta)
            return
        self._cursor = (self._cursor + delta) % len(page.items)

    def _activate_current(self, status: dict[str, str]) -> None:
        item = self._current_item()
        if item.item_type == "submenu" and item.target_page:
            self._current_page_id = item.target_page
            self._cursor = 0
            self._editing_range = False
            self._message = ""
            return
        if item.item_type == "range":
            self._editing_range = not self._editing_range
            self._message = "Adjusting range value" if self._editing_range else ""
            return
        if item.item_type == "toggle" and item.config_path:
            current = bool(self._effective_value(item.config_path))
            self._set_config_value(item, not current)
            return
        if item.item_type == "action" and item.command:
            self._message = self._execute_command(item.command, {"status": status}) or ""

    def _adjust_range(self, delta: int) -> None:
        item = self._current_item()
        if item.item_type != "range" or not item.config_path:
            return
        current = float(self._effective_value(item.config_path))
        step = item.step or 1.0
        next_value = current + (delta * step)
        if item.min_value is not None:
            next_value = max(item.min_value, next_value)
        if item.max_value is not None:
            next_value = min(item.max_value, next_value)
        if item.value_type == "int":
            typed: int | float = int(round(next_value))
        else:
            typed = round(next_value, 3)
        self._set_config_value(item, typed)

    def _effective_value(self, config_path: str) -> Any:
        if config_path in self._staged_overrides:
            return self._staged_overrides[config_path]
        return _get_by_path(self._config, config_path)

    def _set_config_value(self, item: MenuItem, value: Any) -> None:
        if not item.config_path:
            return
        if item.apply_mode == "staged":
            self._staged_overrides[item.config_path] = value
            self._message = f"Staged change ({len(self._staged_overrides)} unsaved)"
            return
        _set_by_path(self._config, item.config_path, value)
        if item.on_change_command:
            self._execute_command(item.on_change_command, {"value": value})
        self._message = f"Updated {item.label}"
        if self._on_runtime_refresh:
            self._on_runtime_refresh()

    def _execute_command(self, command_id: str, payload: dict[str, Any]) -> str | None:
        if command_id == "config.save":
            return self._save_config()
        if command_id == "config.reload":
            return self._reload_config()
        handler = self._command_registry.get(command_id)
        if not handler:
            return f"Unknown command: {command_id}"
        result = handler(payload)
        return result

    def _save_config(self) -> str:
        for path, value in self._staged_overrides.items():
            _set_by_path(self._config, path, value)
        self._staged_overrides.clear()
        self._config_path.parent.mkdir(parents=True, exist_ok=True)
        self._config_path.write_text(yaml.safe_dump(self._config, sort_keys=False), encoding="utf-8")
        if self._on_runtime_refresh:
            self._on_runtime_refresh()
        return "Config saved"

    def _reload_config(self) -> str:
        raw = yaml.safe_load(self._config_path.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            return "Reload failed: invalid config file"
        self._config.clear()
        self._config.update(raw)
        self._staged_overrides.clear()
        if self._on_runtime_refresh:
            self._on_runtime_refresh()
        return "Config reloaded"

    def render_model(self, status: dict[str, str]) -> MenuRenderModel:
        page = self._current_page()
        title = page.title
        if self.dirty:
            title = f"{title}  * unsaved"

        rows: list[MenuRenderRow] = []
        for index, item in enumerate(page.items):
            rows.append(
                MenuRenderRow(
                    item_id=item.item_id,
                    label=item.label,
                    value_text=self._value_text(item, status),
                    selected=index == self._cursor,
                    item_type=item.item_type,
                    icon=item.icon,
                )
            )

        footer = "Turn: Move  Press: Select"
        if self._editing_range:
            footer = "Turn: Adjust  Press: Done"
        return MenuRenderModel(
            page_id=page.page_id,
            title=title,
            rows=rows,
            footer=footer,
            message=self._message,
        )

    def _value_text(self, item: MenuItem, status: dict[str, str]) -> str:
        if item.item_type == "submenu":
            return ">"
        if item.item_type == "info":
            if not item.status_key:
                return ""
            return f"{status.get(item.status_key, 'unknown')}"
        if item.item_type == "toggle":
            if not item.config_path:
                return ""
            value = bool(self._effective_value(item.config_path))
            staged = " (staged)" if item.config_path in self._staged_overrides else ""
            return f"{'ON' if value else 'OFF'}{staged}"
        if item.item_type == "range":
            if not item.config_path:
                return ""
            value = self._effective_value(item.config_path)
            staged = " (staged)" if item.config_path in self._staged_overrides else ""
            editing = " [edit]" if self._editing_range and item == self._current_item() else ""
            if item.value_type == "float":
                return f"{float(value):.2f}{staged}{editing}"
            return f"{int(value)}{staged}{editing}"
        return ""
