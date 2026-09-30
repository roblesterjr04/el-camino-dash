#pragma once

// 256×64 framebuffer in 16 grey levels (the SSD1322's), with the few primitives the console's screens use: boxes,
// rounded boxes, bars, lines, anti-aliased text in LVGL-format fonts (4 bpp, or 1 bpp for the pixel font) and the
// prototype's small line icons. No Arduino or LVGL runtime: it only reads LVGL font data, so the same code renders
// the screens on the host (tools/console_preview).

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

enum class TextAlign : uint8_t { Left, Center, Right };

class OledGfx {
 public:
  static const int W = 256, H = 64;

  void clear() { for (auto &p : fb_) p = 0; }
  uint8_t *pixels() { return fb_; }
  const uint8_t *pixels() const { return fb_; }

  /** Everything drawn after this is clipped to the rectangle (reset with no_clip). */
  void clip(int x, int y, int w, int h);
  void no_clip() { clip(0, 0, W, H); }
  /** Shift everything drawn after this (page slide). */
  void offset(int dx, int dy) { ox_ = dx; oy_ = dy; }

  void box(int x, int y, int w, int h, int lv);
  void rect(int x, int y, int w, int h, int lv);                // 1 px outline
  void rbox(int x, int y, int w, int h, int r, int lv);         // filled, rounded corners
  void rrect(int x, int y, int w, int h, int r, int lv);        // outline, rounded corners
  void bar(int x, int y, int w, int h, float frac, int on, int off);
  void line(float x0, float y0, float x1, float y1, int lv, float width = 1.2f);
  void disc(float cx, float cy, float r, int lv);
  void ring(float cx, float cy, float r, int lv, float width = 1.2f);
  void tri(float x0, float y0, float x1, float y1, float x2, float y2, int lv);
  /** 48×48 grey image, 4 bits per pixel, row-major, left pixel in the high nibble (the bridge's GRAY48 variant). */
  void gray48(int x, int y, const uint8_t *img);

  /** Text on a baseline at y. max_w > 0 cuts it with "…" to fit. Returns the drawn width. */
  int text(const char *s, int x, int y, const lv_font_t *f, int lv, TextAlign a = TextAlign::Left, int max_w = 0);
  static int text_width(const char *s, const lv_font_t *f);
  /** s cut (with "…") to fit max_w, into out. */
  static void fit(const char *s, const lv_font_t *f, int max_w, char *out, size_t n);

  /** The prototype's 12 px line icons: note, play, pause, warn, gps, bt, spk, ext, round, phone. */
  void icon(const char *name, int x, int y, int lv);
  /** The Chevrolet bowtie outline at level lv, filled faintly at level fill (0 = no fill). */
  void bowtie(int lv, int fill = 0);
  /** Brighten lit pixels in a diagonal band centred at x = pos (a moving highlight). */
  void sheen(float pos, float width, float gain, float lift = 0);
  /** Filled polygon (even-odd), anti-aliased, points offset by (ox, oy). */
  void poly(const float (*p)[2], int n, float ox, float oy, int lv);

 private:
  void plot(int x, int y, int lv, int alpha = 15);  // alpha 0..15 blends toward lv
  void span(int x0, int x1, int y, int lv);
  void glyph(const lv_font_t *f, uint32_t cp, uint32_t next, int x, int baseline, int lv, int *adv);

  uint8_t fb_[W * H] = {0};
  int cx0_ = 0, cy0_ = 0, cx1_ = W, cy1_ = H;
  int ox_ = 0, oy_ = 0;
};
