#pragma once

#include <stdint.h>

#include <lvgl.h>

/**
 * Colours and fonts of the LVGL overlays (menu, call card) per cluster theme, so every overlay looks the same.
 * Theme numbering as on the cluster bus: 0 Classic, 1 Fallout, 2 Modern.
 */
struct UiTheme {
  lv_color_t bg, text, dim, faint, accent, ink, surface, line, ok, bad;
  const lv_font_t *label, *title, *body, *sub;  // large, heading, body, caption
};

UiTheme ui_theme(uint8_t theme);
