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


_PHY_CLK_MARK = "// EL_CAMINO_DSI_PHY_CLK"


def apply_dsi_phy_clk_fix(libdeps: Path) -> None:
    """GFX asks for MIPI_DSI_PHY_CLK_SRC_DEFAULT, which IDF keeps as the pre-v3 PLL_F20M source. Rev ≥ 3 P4 silicon
    (the 4C board) has no such PLL reference and esp_lcd_new_dsi_bus() aborts; use XTAL there, keep the old source
    on the ES chips (3.4C)."""
    if not libdeps.is_dir():
        return
    for cpp in libdeps.rglob("Arduino_ESP32DSIPanel.cpp"):
        if "GFX Library for Arduino" not in str(cpp):
            continue
        text = cpp.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
        if _PHY_CLK_MARK in text:
            continue
        anchor = '#include "Arduino_ESP32DSIPanel.h"\n'
        field = ".phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,"
        if anchor not in text or field not in text:
            print("pre: WARN DSI phy clock fix: pattern missing in %s (skip)" % cpp)
            continue
        text = text.replace(
            anchor,
            anchor + "\n" + _PHY_CLK_MARK + "\n#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3\n"
            "#define ELC_DSI_PHY_CLK_SRC MIPI_DSI_PHY_CLK_SRC_DEFAULT\n#else\n"
            "#define ELC_DSI_PHY_CLK_SRC MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT\n#endif\n",
            1,
        )
        text = text.replace(field, ".phy_clk_src = ELC_DSI_PHY_CLK_SRC,", 1)
        cpp.write_text(text, encoding="utf-8", newline="\n")
        print("pre: patched %s (DSI PHY PLL reference for rev >= 3 silicon)" % cpp)
