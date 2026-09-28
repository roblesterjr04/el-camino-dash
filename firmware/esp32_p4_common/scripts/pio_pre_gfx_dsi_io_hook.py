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
        # GFX <= 1.6.x included esp_lcd_panel_io.h directly; 1.6.5 gets it via its own header.
        inc = next(
            (i for i in ("#include <esp_lcd_panel_io.h>\n", '#include "Arduino_ESP32DSIPanel.h"\n') if i in normalized),
            None,
        )
        if inc is None:
            print("pre: WARN DSI io hook: no include anchor in %s (skip)" % cpp)
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


_FB_MARK = "// EL_CAMINO_DSI_FB_HOOK"


def apply_dsi_fb_hook(libdeps: Path) -> None:
    """Make the DPI framebuffer count a build flag (ELC_DSI_NUM_FBS, default 1) and keep the panel
    handle in g_elc_dpi_panel, so the LVGL screen layer can double-buffer and flip on refresh."""
    if not libdeps.is_dir():
        return
    for cpp in libdeps.rglob("Arduino_ESP32DSIPanel.cpp"):
        if "GFX Library for Arduino" not in str(cpp):
            continue
        text = cpp.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
        if _FB_MARK in text:
            continue
        anchor = '#include "Arduino_ESP32DSIPanel.h"\n'
        create = "ESP_ERROR_CHECK(esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &_panel_handle));"
        if anchor not in text or ".num_fbs = 1," not in text or create not in text:
            print("pre: WARN DSI fb hook: pattern missing in %s (skip)" % cpp)
            continue
        text = text.replace(
            anchor,
            anchor + "\n#ifndef ELC_DSI_NUM_FBS\n#define ELC_DSI_NUM_FBS 1\n#endif\n"
            "extern esp_lcd_panel_handle_t g_elc_dpi_panel; " + _FB_MARK + "\n",
            1,
        )
        text = text.replace(".num_fbs = 1,", ".num_fbs = ELC_DSI_NUM_FBS,", 1)
        text = text.replace(create, create + "\n  g_elc_dpi_panel = _panel_handle;", 1)
        cpp.write_text(text, encoding="utf-8", newline="\n")
        print("pre: patched %s (DSI framebuffer count + panel handle)" % cpp)
