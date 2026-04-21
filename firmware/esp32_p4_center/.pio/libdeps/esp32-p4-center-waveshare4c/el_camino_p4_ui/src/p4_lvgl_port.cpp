#include "p4_lvgl_port.h"

#include "p4_round_mask.h"

#include <Arduino.h>
#include <canvas/Arduino_Canvas.h>
#include <lvgl.h>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include <esp_task_wdt.h>
#endif

static Arduino_GFX *s_gfx = nullptr;
static Arduino_Canvas *s_overlay_canvas = nullptr;
static bool s_overlay_round_mask = false;

enum FlushMode : uint8_t {
  FLUSH_DIRECT = 0,
  FLUSH_OVERLAY_CANVAS,
};

static FlushMode s_flush_mode = FLUSH_DIRECT;

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_disp_t *s_disp = nullptr;

static lv_color_t *s_buf1 = nullptr;
static size_t s_buf_pixels = 0;
static int16_t s_hor = 0;
static int16_t s_ver = 0;

static void *alloc_draw(size_t bytes) {
#if defined(ESP_PLATFORM)
  void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return p;
#else
  return malloc(bytes);
#endif
}

static void free_draw(void *p) {
  if (!p) {
    return;
  }
#if defined(ESP_PLATFORM)
  heap_caps_free(p);
#else
  free(p);
#endif
}

static void flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  (void)disp_drv;
  if (!s_gfx) {
    lv_disp_flush_ready(disp_drv);
    return;
  }

  const int32_t x = area->x1;
  const int32_t y = area->y1;
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;

  if (s_flush_mode == FLUSH_OVERLAY_CANVAS && s_overlay_canvas) {
    uint16_t *fb = s_overlay_canvas->getFramebuffer();
    if (fb && w > 0 && h > 0) {
      const uint16_t *src = (const uint16_t *)color_p;
      for (int32_t row = 0; row < h; row++) {
        memcpy(fb + (y + row) * s_hor + x, src + row * (size_t)w, (size_t)w * sizeof(uint16_t));
      }
      if (s_overlay_round_mask) {
        p4_rgb565_mask_outside_circle(fb, s_hor, s_ver, 0x0000);
      }
      s_gfx->draw16bitRGBBitmap(0, 0, fb, s_hor, s_ver);
    }
  } else {
    s_gfx->draw16bitRGBBitmap((int16_t)x, (int16_t)y, (uint16_t *)color_p, (int16_t)w, (int16_t)h);
  }

#if defined(ESP_PLATFORM)
  /* Full-screen RGB565 blit + DSI cache sync can exceed the idle task WDT window. */
  esp_task_wdt_reset();
#endif

  lv_disp_flush_ready(disp_drv);
}

static void release_buffer(void) {
  free_draw(s_buf1);
  s_buf1 = nullptr;
  s_buf_pixels = 0;
}

static bool ensure_buffer(int16_t w, int16_t h) {
  const size_t need = (size_t)w * (size_t)h;
  if (need == 0) {
    return false;
  }
  if (s_buf1 && need == s_buf_pixels && w == s_hor && h == s_ver) {
    return true;
  }
  release_buffer();
  s_buf1 = (lv_color_t *)alloc_draw(need * sizeof(lv_color_t));
  if (!s_buf1) {
    return false;
  }
  s_buf_pixels = need;
  s_hor = w;
  s_ver = h;
  return true;
}

bool p4_lvgl_init(Arduino_GFX *gfx) {
  if (!gfx) {
    return false;
  }
  static bool lv_core = false;
  if (!lv_core) {
    lv_init();
    lv_core = true;
  }

  s_gfx = gfx;
  const int16_t w = gfx->width();
  const int16_t h = gfx->height();
  if (!ensure_buffer(w, h)) {
    return false;
  }

  if (!s_disp) {
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, nullptr, (uint32_t)s_buf_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = w;
    s_disp_drv.ver_res = h;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.full_refresh = 1;

    s_disp = lv_disp_drv_register(&s_disp_drv);
    if (!s_disp) {
      release_buffer();
      return false;
    }

    /* Match cluster / emulator dark chrome — avoid BLUE_GREY default (reads as a big “blue screen”). */
    const lv_color_t prim = lv_color_make(0x18, 0x16, 0x14);
    const lv_color_t sec = lv_palette_main(LV_PALETTE_AMBER);
    const lv_theme_t *th = lv_theme_default_init(s_disp, prim, sec, true, &lv_font_montserrat_14);
    lv_disp_set_theme(s_disp, const_cast<lv_theme_t *>(th));
  }

  s_flush_mode = FLUSH_DIRECT;
  s_overlay_canvas = nullptr;
  s_overlay_round_mask = false;
  return true;
}

void p4_lvgl_set_display_size(int16_t w, int16_t h) {
  (void)w;
  (void)h;
}

void p4_lvgl_handler(void) {
  lv_timer_handler();
}

void p4_lvgl_set_overlay_flush(Arduino_GFX *gfx, Arduino_Canvas *canvas, bool apply_round_mask) {
  s_gfx = gfx;
  s_overlay_canvas = canvas;
  s_overlay_round_mask = apply_round_mask;
  s_flush_mode = FLUSH_OVERLAY_CANVAS;
}

void p4_lvgl_set_direct_flush(Arduino_GFX *gfx) {
  s_gfx = gfx;
  s_overlay_canvas = nullptr;
  s_overlay_round_mask = false;
  s_flush_mode = FLUSH_DIRECT;
}

void p4_lvgl_clear_flush_mode(void) {
  s_overlay_canvas = nullptr;
  s_overlay_round_mask = false;
  s_flush_mode = FLUSH_DIRECT;
}
