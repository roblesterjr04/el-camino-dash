from __future__ import annotations

import json
from pathlib import Path
from typing import Any


def _deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    merged: dict[str, Any] = dict(base)
    for key, value in override.items():
        existing = merged.get(key)
        if isinstance(existing, dict) and isinstance(value, dict):
            merged[key] = _deep_merge(existing, value)
        else:
            merged[key] = value
    return merged


def _load_theme_data_recursive(theme_root: Path, theme_name: str, seen: set[str]) -> dict[str, Any]:
    if theme_name in seen:
        return {}
    seen.add(theme_name)
    theme_path = theme_root / theme_name / "theme.json"
    if not theme_path.exists():
        return {}
    try:
        raw = json.loads(theme_path.read_text(encoding="utf-8"))
    except (OSError, ValueError, json.JSONDecodeError):
        return {}
    if not isinstance(raw, dict):
        return {}

    extends_name = raw.get("extends")
    if not isinstance(extends_name, str) or not extends_name.strip():
        return raw

    parent = _load_theme_data_recursive(theme_root, extends_name.strip(), seen)
    child = dict(raw)
    child.pop("extends", None)
    return _deep_merge(parent, child)


def load_theme_data(theme_root: Path, theme_name: str) -> dict[str, Any]:
    return _load_theme_data_recursive(theme_root, theme_name, set())


def apply_center_dial_persisted(theme_data: dict[str, Any], persisted: str | None) -> dict[str, Any]:
    """Merge NVS-style center dial preference over theme.json (same shape as menu toggle)."""
    if not persisted:
        return theme_data
    mode = persisted.strip().lower()
    if mode not in ("tach", "clock"):
        return theme_data
    out = dict(theme_data)
    mode_cfg = out.get("center_dial_mode", "tach")
    if isinstance(mode_cfg, str):
        out["center_dial_mode"] = {"_default": mode_cfg, "gauge_center": mode}
    elif isinstance(mode_cfg, dict):
        updated = dict(mode_cfg)
        updated["gauge_center"] = mode
        out["center_dial_mode"] = updated
    else:
        out["center_dial_mode"] = {"_default": "tach", "gauge_center": mode}
    return out
