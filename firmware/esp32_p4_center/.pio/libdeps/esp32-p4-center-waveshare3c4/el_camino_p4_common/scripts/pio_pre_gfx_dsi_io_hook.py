# Patch moononournation GFX Arduino_ESP32DSIPanel.cpp so we retain esp_lcd_panel_io_handle_t for
# runtime SH8601 brightness (DCS 0x51). Idempotent; safe to run every build.
from __future__ import annotations

from pathlib import Path

_HOOK_MARK = "// EL_CAMINO_DSI_IO_HOOK"


def apply_dsi_panel_io_hook(libdeps: Path) -> None:
    if not libdeps.is_dir():
        return
    needle_lf = "ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io_handle));"
    for cpp in libdeps.rglob("Arduino_ESP32DSIPanel.cpp"):
        if "GFX Library for Arduino" not in str(cpp):
            continue
        text = cpp.read_text(encoding="utf-8", errors="replace")
        if _HOOK_MARK in text:
            continue
        normalized = text.replace("\r\n", "\n")
        if needle_lf not in normalized:
            print("pre: WARN DSI io hook: pattern missing in %s (skip)" % cpp)
            continue
        inc = "#include <esp_lcd_panel_io.h>\n"
        if inc not in normalized:
            print("pre: WARN DSI io hook: include not found in %s (skip)" % cpp)
            continue
        normalized = normalized.replace(
            inc,
            inc + "\nextern esp_lcd_panel_io_handle_t g_elc_dsi_panel_io;\n",
            1,
        )
        normalized = normalized.replace(
            needle_lf + "\n",
            needle_lf + "\n  " + _HOOK_MARK + "\n  g_elc_dsi_panel_io = io_handle;\n",
            1,
        )
        cpp.write_text(normalized, encoding="utf-8", newline="\n")
        print("pre: patched %s (DSI panel_io hook for illumination)" % cpp)
