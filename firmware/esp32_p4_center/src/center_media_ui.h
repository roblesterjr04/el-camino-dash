#pragma once

#include <Arduino_GFX_Library.h>
#include <stdint.h>

struct CenterGaugeInputs;

void center_draw_media_screen(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, const CenterGaugeInputs &in);

/** Invalidate cached media frame state when leaving the media view. */
void center_media_lvgl_teardown(void);

/** Legacy no-op (media is drawn with Arduino_GFX; overlay paints on top). */
void center_media_lvgl_sync_overlay(bool hide_for_overlay);
