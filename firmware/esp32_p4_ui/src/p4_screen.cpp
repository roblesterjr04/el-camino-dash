#include "p4_screen.h"

#include <canvas/Arduino_Canvas.h>
#include <display/Arduino_DSI_Display.h>

#if defined(ESP_PLATFORM)
#include <esp_cache.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_rom_crc.h>
#include <esp_task_wdt.h>
#endif

#if defined(ESP_PLATFORM)
extern esp_lcd_panel_handle_t g_elc_dpi_panel;  // display_setup.cpp, set by the patched GFX DSI panel
#endif

namespace {

/** Canvas whose flush() only marks the LVGL layer dirty (the base class would blit to the panel). */
class LegacyLayer : public Arduino_Canvas {
 public:
  LegacyLayer(int16_t w, int16_t h, Arduino_G *out) : Arduino_Canvas(w, h, out, 0, 0, 0) {}
  void flush(bool) override { p4_screen_legacy_dirty(); }
};

Arduino_DSI_Display *s_panel = nullptr;
LegacyLayer *s_legacy = nullptr;
uint16_t *s_fb = nullptr;  // framebuffer the panel is showing (front)

// Double buffering: LVGL draws into the back buffer; the panel flips to it after a full refresh, then
// the rows LVGL touched are copied into the new back buffer so both stay identical.
bool s_double = false;
uint16_t *s_fbs[2] = {nullptr, nullptr};
SemaphoreHandle_t s_flip_done = nullptr;
constexpr int kMaxDirty = 32;
lv_area_t s_dirty[kMaxDirty];
int s_ndirty = 0;
bool s_dirty_overflow = false;
uint32_t s_flips = 0;
int16_t s_w = 0;
int16_t s_h = 0;

lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_disp_drv;
lv_disp_t *s_disp = nullptr;
lv_indev_drv_t s_indev_drv;
lv_img_dsc_t s_legacy_dsc;
lv_obj_t *s_legacy_img = nullptr;
lv_obj_t *s_ui = nullptr;
bool s_legacy_dirty = true;

// Change detection on the legacy layer: one hash per band of rows.
constexpr int kBandRows = 16;
constexpr int kMaxBands = 128;
uint32_t s_band_hash[kMaxBands];
bool s_band_valid = false;

int16_t s_tx = 0;
int16_t s_ty = 0;
bool s_tdown = false;

// stats
uint32_t s_stat_t0 = 0;
uint32_t s_frames = 0;
uint32_t s_rows = 0;
uint64_t s_lvgl_us = 0;
uint64_t s_app_us = 0;
uint32_t s_bands_changed = 0;
P4ScreenStats s_stats{};

void writeback_rows(uint16_t *buf, int32_t y1, int32_t y2) {
#if defined(ESP_PLATFORM)
  esp_cache_msync(buf + (size_t)y1 * (size_t)s_w, (size_t)(y2 - y1 + 1) * (size_t)s_w * sizeof(uint16_t),
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#endif
}

#if defined(ESP_PLATFORM)
bool IRAM_ATTR on_fb_complete(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_flip_done, &woken);
  return woken == pdTRUE;
}
#endif

void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  // Direct mode: LVGL already drew into the buffer; push those rows out of the cache for the DMA.
  uint16_t *buf = (uint16_t *)color_p;
  const int32_t y1 = area->y1 < 0 ? 0 : area->y1;
  const int32_t y2 = area->y2 >= s_h ? s_h - 1 : area->y2;
  if (y2 >= y1) {
    writeback_rows(buf, y1, y2);
    s_rows += (uint32_t)(y2 - y1 + 1);
    if (s_double) {
      if (s_ndirty < kMaxDirty) {
        s_dirty[s_ndirty++] = *area;
      } else {
        s_dirty_overflow = true;
      }
    }
  }
#if defined(ESP_PLATFORM)
  if (s_double && lv_disp_flush_is_last(drv)) {
    // Flip: the panel starts showing `buf` at the next frame; wait until the old front is free.
    xSemaphoreTake(s_flip_done, 0);
    esp_lcd_panel_draw_bitmap(g_elc_dpi_panel, 0, 0, s_w, s_h, buf);
    xSemaphoreTake(s_flip_done, pdMS_TO_TICKS(60));
    s_flips++;
    uint16_t *other = (buf == s_fbs[0]) ? s_fbs[1] : s_fbs[0];
    s_fb = buf;
    // Bring the new back buffer up to date with what LVGL just drew.
    if (s_dirty_overflow) {
      memcpy(other, buf, (size_t)s_w * (size_t)s_h * sizeof(uint16_t));
      writeback_rows(other, 0, s_h - 1);
    } else {
      for (int i = 0; i < s_ndirty; i++) {
        const lv_area_t &a = s_dirty[i];
        const int32_t ax1 = a.x1 < 0 ? 0 : a.x1, ax2 = a.x2 >= s_w ? s_w - 1 : a.x2;
        const int32_t ay1 = a.y1 < 0 ? 0 : a.y1, ay2 = a.y2 >= s_h ? s_h - 1 : a.y2;
        if (ax2 < ax1 || ay2 < ay1) {
          continue;
        }
        const size_t row_bytes = (size_t)(ax2 - ax1 + 1) * sizeof(uint16_t);
        for (int32_t y = ay1; y <= ay2; y++) {
          const size_t off = (size_t)y * (size_t)s_w + (size_t)ax1;
          memcpy(other + off, buf + off, row_bytes);
        }
        writeback_rows(other, ay1, ay2);
      }
    }
    s_ndirty = 0;
    s_dirty_overflow = false;
  }
#endif
  lv_disp_flush_ready(drv);
}

void touch_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
  data->point.x = s_tx;
  data->point.y = s_ty;
  data->state = s_tdown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

Arduino_GFX *p4_screen_begin(Arduino_GFX *panel) {
  if (!panel) {
    return nullptr;
  }
  s_panel = static_cast<Arduino_DSI_Display *>(panel);
  s_fb = s_panel->getFramebuffer();
  s_w = panel->width();
  s_h = panel->height();
  if (!s_fb || s_w <= 0 || s_h <= 0) {
    return panel;  // no framebuffer: keep drawing straight to the panel
  }

  s_legacy = new LegacyLayer(s_w, s_h, panel);
  if (!s_legacy->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    delete s_legacy;
    s_legacy = nullptr;
    return panel;
  }
  // Start from what the panel shows now (boot splash) so the first LVGL frame doesn't flash black.
  memcpy(s_legacy->getFramebuffer(), s_fb, (size_t)s_w * (size_t)s_h * sizeof(uint16_t));

  lv_init();
#if defined(ESP_PLATFORM)
  void *fb0 = nullptr, *fb1 = nullptr;
  if (g_elc_dpi_panel && esp_lcd_dpi_panel_get_frame_buffer(g_elc_dpi_panel, 2, &fb0, &fb1) == ESP_OK && fb0 && fb1) {
    s_flip_done = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_frame_buf_complete = on_fb_complete;
    if (s_flip_done && esp_lcd_dpi_panel_register_event_callbacks(g_elc_dpi_panel, &cbs, nullptr) == ESP_OK) {
      s_fbs[0] = (uint16_t *)fb0;  // showing the boot splash
      s_fbs[1] = (uint16_t *)fb1;
      memcpy(s_fbs[1], s_fbs[0], (size_t)s_w * (size_t)s_h * sizeof(uint16_t));
      writeback_rows(s_fbs[1], 0, s_h - 1);
      s_fb = s_fbs[0];
      s_double = true;
    }
  }
#endif
  if (s_double) {
    lv_disp_draw_buf_init(&s_draw_buf, s_fbs[1], s_fbs[0], (uint32_t)s_w * (uint32_t)s_h);  // draw into the back first
  } else {
    lv_disp_draw_buf_init(&s_draw_buf, s_fb, nullptr, (uint32_t)s_w * (uint32_t)s_h);
  }
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = s_w;
  s_disp_drv.ver_res = s_h;
  s_disp_drv.flush_cb = flush_cb;
  s_disp_drv.draw_buf = &s_draw_buf;
  s_disp_drv.direct_mode = 1;
  s_disp = lv_disp_drv_register(&s_disp_drv);

  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = touch_cb;
  lv_indev_drv_register(&s_indev_drv);

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  s_legacy_dsc = {};
  s_legacy_dsc.header.always_zero = 0;
  s_legacy_dsc.header.w = (uint32_t)s_w;
  s_legacy_dsc.header.h = (uint32_t)s_h;
  s_legacy_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  s_legacy_dsc.data_size = (uint32_t)s_w * (uint32_t)s_h * LV_COLOR_SIZE / 8;
  s_legacy_dsc.data = (const uint8_t *)s_legacy->getFramebuffer();
  s_legacy_img = lv_img_create(scr);
  lv_img_set_src(s_legacy_img, &s_legacy_dsc);
  lv_obj_set_pos(s_legacy_img, 0, 0);
  lv_obj_clear_flag(s_legacy_img, LV_OBJ_FLAG_CLICKABLE);

  s_ui = lv_obj_create(scr);
  lv_obj_remove_style_all(s_ui);
  lv_obj_set_size(s_ui, s_w, s_h);
  lv_obj_clear_flag(s_ui, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_ui, LV_OBJ_FLAG_CLICKABLE);

  s_stat_t0 = millis();
  Serial.printf("[screen] LVGL direct mode %dx%d, %s buffered, legacy layer %p\n", (int)s_w, (int)s_h,
                s_double ? "double" : "single", (void *)s_legacy->getFramebuffer());
  return s_legacy;
}

void p4_screen_legacy_dirty() { s_legacy_dirty = true; }

void p4_screen_note_app_us(uint32_t us) { s_app_us += us; }

/** Invalidate only the row bands of the legacy layer whose content changed since the last frame. */
static void invalidate_changed_bands() {
  const uint32_t *px = (const uint32_t *)s_legacy->getFramebuffer();
  const int words_per_row = s_w / 2;
  const int bands = (s_h + kBandRows - 1) / kBandRows;
  int run_start = -1;
  for (int b = 0; b <= bands; b++) {
    bool changed = false;
    if (b < bands) {
      const int y0 = b * kBandRows;
      const int y1 = (y0 + kBandRows < s_h) ? y0 + kBandRows : s_h;
      const uint32_t *p = px + (size_t)y0 * (size_t)words_per_row;
      const uint32_t *e = px + (size_t)y1 * (size_t)words_per_row;
      uint32_t h = 2166136261u;
      while (p < e) {
        h = (h ^ p[0]) * 16777619u;
        h = (h ^ p[1]) * 16777619u;
        h = (h ^ p[2]) * 16777619u;
        h = (h ^ p[3]) * 16777619u;
        p += 4;
      }
      changed = !s_band_valid || b >= kMaxBands || s_band_hash[b] != h;
      if (b < kMaxBands) {
        s_band_hash[b] = h;
      }
    }
    if (changed) {
      s_bands_changed++;
    }
    if (changed && run_start < 0) {
      run_start = b;
    } else if (!changed && run_start >= 0) {
      lv_area_t a;
      a.x1 = 0;
      a.x2 = s_w - 1;
      a.y1 = run_start * kBandRows;
      a.y2 = (b * kBandRows < s_h ? b * kBandRows : s_h) - 1;
      lv_obj_invalidate_area(s_legacy_img, &a);
      run_start = -1;
    }
  }
  s_band_valid = true;
}

void p4_screen_touch(int16_t x, int16_t y, bool down) {
  s_tx = x;
  s_ty = y;
  s_tdown = down;
}

void p4_screen_frame() {
  if (!s_disp) {
    return;
  }
  const int64_t t0 = esp_timer_get_time();
  if (s_legacy_dirty && s_legacy_img) {
    invalidate_changed_bands();
    s_legacy_dirty = false;
  }
  lv_timer_handler();
  lv_refr_now(s_disp);  // draw this loop's legacy frame now, not on LVGL's 30 ms timer
  s_lvgl_us += (uint64_t)(esp_timer_get_time() - t0);
  s_frames++;

  const uint32_t now = millis();
  if (now - s_stat_t0 >= 1000) {
    const float secs = (float)(now - s_stat_t0) / 1000.0f;
    s_stats.fps = (float)s_frames / secs;
    s_stats.lvgl_ms = s_frames ? (float)s_lvgl_us / 1000.0f / (float)s_frames : 0.0f;
    s_stats.app_ms = s_frames ? (float)s_app_us / 1000.0f / (float)s_frames : 0.0f;
    s_app_us = 0;
    s_stats.bands_changed = s_frames ? (float)s_bands_changed / (float)s_frames : 0.0f;
    s_bands_changed = 0;
    s_stats.flushed_rows = s_rows;
    s_stats.double_buffered = s_double;
    s_stats.flips = s_flips;
    s_flips = 0;
    s_frames = 0;
    s_rows = 0;
    s_lvgl_us = 0;
    s_stat_t0 = now;
  }
}

lv_obj_t *p4_screen_ui_parent() { return s_ui ? s_ui : lv_scr_act(); }

P4ScreenStats p4_screen_stats() { return s_stats; }

void p4_screen_shot(Stream &out, HardwareSerial *uart, uint32_t fast_baud, uint32_t normal_baud, bool legacy_layer) {
  const uint16_t *fb = legacy_layer ? (s_legacy ? s_legacy->getFramebuffer() : nullptr) : s_fb;
  const int16_t w = s_w;
  const int16_t h = s_h;
  if (!fb) {
    out.println("SHOT ERR no framebuffer");
    return;
  }
  const size_t bytes = (size_t)w * (size_t)h * sizeof(uint16_t);
  if (uart && fast_baud) {
    out.printf("SHOT BAUD %lu\n", (unsigned long)fast_baud);
    out.flush();
    delay(30);
    uart->updateBaudRate(fast_baud);
    delay(300);  // host reopens at the new rate
  }
  out.printf("SHOT %d %d RGB565LE %u\n", (int)w, (int)h, (unsigned)bytes);
  const uint8_t *p = (const uint8_t *)fb;
  uint32_t crc = 0;
  const size_t chunk = 4096;
  for (size_t off = 0; off < bytes; off += chunk) {
    const size_t n = (bytes - off) < chunk ? (bytes - off) : chunk;
#if defined(ESP_PLATFORM)
    crc = esp_rom_crc32_le(crc, p + off, n);
    esp_task_wdt_reset();
#endif
    out.write(p + off, n);
  }
  out.printf("\nSHOT END %08lx\n", (unsigned long)crc);
  out.flush();
  if (uart && fast_baud) {
    delay(30);
    uart->updateBaudRate(normal_baud);
  }
}
