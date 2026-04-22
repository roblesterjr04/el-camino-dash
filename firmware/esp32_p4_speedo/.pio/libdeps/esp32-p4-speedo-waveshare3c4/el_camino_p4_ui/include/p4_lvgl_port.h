#pragma once

#include <Arduino_GFX_Library.h>
#include <stdint.h>

class Arduino_Canvas;

bool p4_lvgl_init(Arduino_GFX *gfx);
void p4_lvgl_set_display_size(int16_t w, int16_t h);
void p4_lvgl_handler(void);
void p4_lvgl_set_overlay_flush(Arduino_GFX *gfx, Arduino_Canvas *canvas, bool apply_round_mask);
void p4_lvgl_set_direct_flush(Arduino_GFX *gfx);
void p4_lvgl_clear_flush_mode(void);
