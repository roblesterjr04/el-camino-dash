// Host stand-in for esp32_p4_ui/include/p4_screen.h (the real one needs Arduino_GFX): overlays go on the screen.
#pragma once
#include <lvgl.h>
static inline lv_obj_t *p4_screen_ui_parent() { return lv_scr_act(); }
static inline void p4_screen_set_tear_free(bool) {}
