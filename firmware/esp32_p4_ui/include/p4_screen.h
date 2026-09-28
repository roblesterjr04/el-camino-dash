#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

/**
 * LVGL owns the panel. LVGL renders in direct mode straight into the DSI framebuffer and only the
 * rows it touched are written back from cache, so a small change (a needle, a label) costs little.
 *
 * Existing Arduino_GFX drawing code keeps working: it draws into an off-screen "legacy layer" canvas
 * (same size as the panel), which is the bottom object on the LVGL screen. Anything created with
 * LVGL on p4_screen_ui_parent() sits above it.
 *
 * Loop contract: draw the legacy layer (if anything changed), call p4_screen_touch() with the latest
 * touch sample, then p4_screen_frame() once.
 */

/** Takes over the panel. Returns the legacy-layer canvas to hand to existing drawing code. */
Arduino_GFX *p4_screen_begin(Arduino_GFX *panel);

/** The legacy layer may have changed; the next frame compares it band by band and redraws only
 *  what differs (Arduino_Canvas::flush() on the layer also marks this). */
void p4_screen_legacy_dirty();

/** Feed the latest touch sample to LVGL's pointer input. */
void p4_screen_touch(int16_t x, int16_t y, bool down);

/** Run LVGL: timers, input, and redraw of whatever is invalid. */
void p4_screen_frame();

/**
 * Tear-free (default): draw into the hidden buffer, flip on refresh, then sync the other buffer.
 * Off: finished strips are copied straight into the visible buffer (no flip wait, no sync), roughly
 * doubling the frame rate for big moving content such as the menu wheel, at the cost of possible
 * tearing. Turning it back on re-syncs the hidden buffer once.
 */
void p4_screen_set_tear_free(bool on);

/** Parent object for LVGL UI above the legacy layer. */
lv_obj_t *p4_screen_ui_parent();

/** Report how long the app spent drawing the legacy layer this loop (shown by `perf`). */
void p4_screen_note_app_us(uint32_t us);

/** Timing of the last second, for the `perf` console command. */
struct P4ScreenStats {
  float fps;
  float app_ms;           // average legacy-layer drawing time reported by the app
  float lvgl_ms;          // average p4_screen_frame() time (change detection + LVGL render)
  uint32_t flushed_rows;  // rows written back to the panel in the last second
  float bands_changed;    // average legacy-layer bands (16 rows) that changed per frame
  bool double_buffered;   // panel flips between two framebuffers (no tearing)
  uint32_t flips;         // buffer flips in the last second
  float flip_wait_ms;     // per flip: waiting for the panel to let go of the old buffer
  float flip_sync_ms;     // per flip: copying redrawn areas into the other buffer
  float areas_per_flip;
  bool dma_sync;          // buffer sync uses the AXI DMA (else CPU memcpy)
  float sync_rows_per_flip;
};
P4ScreenStats p4_screen_stats();

/**
 * Stream what the panel is showing as raw RGB565 (little-endian), for tools/screenshot.py.
 * If `uart` is given, the port switches to `fast_baud` for the transfer and back afterwards.
 * Protocol: "SHOT BAUD <n>\n" (only when switching), "SHOT <w> <h> RGB565LE <bytes>\n", raw bytes,
 * "\nSHOT END <crc32 hex>\n".
 */
void p4_screen_shot(Stream &out, HardwareSerial *uart, uint32_t fast_baud, uint32_t normal_baud, bool legacy_layer = false);
