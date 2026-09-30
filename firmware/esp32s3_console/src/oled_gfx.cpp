#include "oled_gfx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** Next UTF-8 code point from *s (advances *s); 0 at the end. */
uint32_t utf8_next(const char **s) {
  const uint8_t *p = (const uint8_t *)*s;
  if (!*p) return 0;
  uint32_t c = *p++;
  int n = 0;
  if (c >= 0xF0) { c &= 0x07; n = 3; }
  else if (c >= 0xE0) { c &= 0x0F; n = 2; }
  else if (c >= 0xC0) { c &= 0x1F; n = 1; }
  while (n-- > 0 && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
  *s = (const char *)p;
  return c;
}

uint32_t utf8_peek(const char *s) { return utf8_next(&s); }

/** Distance from (px, py) to the segment a-b. */
float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
  const float dx = bx - ax, dy = by - ay;
  const float l2 = dx * dx + dy * dy;
  float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
  t = clampf(t, 0, 1);
  const float qx = ax + t * dx - px, qy = ay + t * dy - py;
  return sqrtf(qx * qx + qy * qy);
}

}  // namespace

void OledGfx::clip(int x, int y, int w, int h) {
  cx0_ = x < 0 ? 0 : x;
  cy0_ = y < 0 ? 0 : y;
  cx1_ = x + w > W ? W : x + w;
  cy1_ = y + h > H ? H : y + h;
}

void OledGfx::plot(int x, int y, int lv, int alpha) {
  x += ox_;
  y += oy_;
  if (x < cx0_ || x >= cx1_ || y < cy0_ || y >= cy1_ || alpha <= 0) return;
  uint8_t &p = fb_[y * W + x];
  if (alpha >= 15) {
    p = (uint8_t)(lv < 0 ? 0 : (lv > 15 ? 15 : lv));
  } else {
    const int v = p + ((lv - p) * alpha + (lv >= p ? 7 : -7)) / 15;
    p = (uint8_t)(v < 0 ? 0 : (v > 15 ? 15 : v));
  }
}

void OledGfx::span(int x0, int x1, int y, int lv) {
  for (int x = x0; x < x1; x++) plot(x, y, lv);
}

void OledGfx::box(int x, int y, int w, int h, int lv) {
  for (int j = y; j < y + h; j++) span(x, x + w, j, lv);
}

void OledGfx::rect(int x, int y, int w, int h, int lv) {
  span(x, x + w, y, lv);
  span(x, x + w, y + h - 1, lv);
  for (int j = y + 1; j < y + h - 1; j++) {
    plot(x, j, lv);
    plot(x + w - 1, j, lv);
  }
}

void OledGfx::rbox(int x, int y, int w, int h, int r, int lv) {
  for (int j = y; j < y + h; j++) {
    for (int i = x; i < x + w; i++) {
      // Distance into a corner circle (pixel centres), anti-aliased.
      const float px = i + 0.5f, py = j + 0.5f;
      const float cx = px < x + r ? x + r : (px > x + w - r ? x + w - r : px);
      const float cy = py < y + r ? y + r : (py > y + h - r ? y + h - r : py);
      const float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
      plot(i, j, lv, (int)(clampf(r + 0.5f - d, 0, 1) * 15 + 0.5f));
    }
  }
}

void OledGfx::rrect(int x, int y, int w, int h, int r, int lv) {
  for (int j = y; j < y + h; j++) {
    for (int i = x; i < x + w; i++) {
      const float px = i + 0.5f, py = j + 0.5f;
      const float cx = px < x + r ? x + r : (px > x + w - r ? x + w - r : px);
      const float cy = py < y + r ? y + r : (py > y + h - r ? y + h - r : py);
      const bool corner = cx != px || cy != py;
      float cov;
      if (corner) {
        const float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
        cov = clampf(1.0f - fabsf(d - (r - 0.5f)), 0, 1);
      } else {
        cov = (i == x || i == x + w - 1 || j == y || j == y + h - 1) ? 1.0f : 0.0f;
      }
      plot(i, j, lv, (int)(cov * 15 + 0.5f));
    }
  }
}

void OledGfx::bar(int x, int y, int w, int h, float frac, int on, int off) {
  box(x, y, w, h, off);
  const int fw = (int)lroundf(w * clampf(frac, 0, 1));
  if (fw > 0) box(x, y, fw, h, on);
}

void OledGfx::line(float x0, float y0, float x1, float y1, int lv, float width) {
  const float hw = width / 2;
  const int bx0 = (int)floorf(fminf(x0, x1) - hw - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + hw + 1);
  const int by0 = (int)floorf(fminf(y0, y1) - hw - 1), by1 = (int)ceilf(fmaxf(y0, y1) + hw + 1);
  for (int j = by0; j <= by1; j++) {
    for (int i = bx0; i <= bx1; i++) {
      const float d = seg_dist(i + 0.5f, j + 0.5f, x0, y0, x1, y1);
      const float cov = clampf(hw + 0.5f - d, 0, 1);
      if (cov > 0) plot(i, j, lv, (int)(cov * 15 + 0.5f));
    }
  }
}

void OledGfx::disc(float cx, float cy, float r, int lv) {
  for (int j = (int)floorf(cy - r - 1); j <= (int)ceilf(cy + r + 1); j++) {
    for (int i = (int)floorf(cx - r - 1); i <= (int)ceilf(cx + r + 1); i++) {
      const float d = sqrtf((i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy));
      const float cov = clampf(r + 0.5f - d, 0, 1);
      if (cov > 0) plot(i, j, lv, (int)(cov * 15 + 0.5f));
    }
  }
}

void OledGfx::ring(float cx, float cy, float r, int lv, float width) {
  const float hw = width / 2;
  for (int j = (int)floorf(cy - r - hw - 1); j <= (int)ceilf(cy + r + hw + 1); j++) {
    for (int i = (int)floorf(cx - r - hw - 1); i <= (int)ceilf(cx + r + hw + 1); i++) {
      const float d = sqrtf((i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy));
      const float cov = clampf(hw + 0.5f - fabsf(d - r), 0, 1);
      if (cov > 0) plot(i, j, lv, (int)(cov * 15 + 0.5f));
    }
  }
}

void OledGfx::tri(float x0, float y0, float x1, float y1, float x2, float y2, int lv) {
  const int bx0 = (int)floorf(fminf(x0, fminf(x1, x2))), bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
  const int by0 = (int)floorf(fminf(y0, fminf(y1, y2))), by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
  const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
  if (area == 0) return;
  for (int j = by0; j <= by1; j++) {
    for (int i = bx0; i <= bx1; i++) {
      int in = 0;  // 4×4 samples per pixel
      for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
          const float px = i + (sx + 0.5f) / 4, py = j + (sy + 0.5f) / 4;
          const float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
          const float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
          const float w2 = 1 - w0 - w1;
          if (w0 >= 0 && w1 >= 0 && w2 >= 0) in++;
        }
      }
      if (in) plot(i, j, lv, (in * 15 + 8) / 16);
    }
  }
}

void OledGfx::gray48(int x, int y, const uint8_t *img) {
  for (int j = 0; j < 48; j++) {
    for (int i = 0; i < 48; i++) {
      const uint8_t b = img[(j * 48 + i) / 2];
      plot(x + i, y + j, (i & 1) ? (b & 0x0F) : (b >> 4));
    }
  }
}

// ---- text ----

void OledGfx::glyph(const lv_font_t *f, uint32_t cp, uint32_t next, int x, int baseline, int lv, int *adv) {
  lv_font_glyph_dsc_t g;
  if (!lv_font_get_glyph_dsc(f, &g, cp, next)) {
    *adv = f->line_height / 4;  // unknown glyph: a gap
    return;
  }
  *adv = g.adv_w;
  const uint8_t *bmp = lv_font_get_glyph_bitmap(f, cp);
  if (!bmp || g.box_w == 0 || g.box_h == 0) return;
  const int bpp = g.bpp ? g.bpp : 1;
  const int top = baseline - g.box_h - g.ofs_y;
  const int left = x + g.ofs_x;
  const int maxv = (1 << bpp) - 1;
  uint32_t bit = 0;  // glyph bitmaps are packed without row padding
  for (int j = 0; j < g.box_h; j++) {
    for (int i = 0; i < g.box_w; i++, bit += bpp) {
      const uint8_t byte = bmp[bit >> 3];
      const int shift = 8 - bpp - (int)(bit & 7);
      const int v = (byte >> shift) & maxv;
      if (v) plot(left + i, top + j, lv, (v * 15 + maxv / 2) / maxv);
    }
  }
}

int OledGfx::text_width(const char *s, const lv_font_t *f) {
  int w = 0;
  const char *p = s;
  uint32_t c;
  while ((c = utf8_next(&p)) != 0) {
    lv_font_glyph_dsc_t g;
    w += lv_font_get_glyph_dsc(f, &g, c, utf8_peek(p)) ? g.adv_w : f->line_height / 4;
  }
  return w;
}

void OledGfx::fit(const char *s, const lv_font_t *f, int max_w, char *out, size_t n) {
  strncpy(out, s ? s : "", n - 1);
  out[n - 1] = '\0';
  if (max_w <= 0 || text_width(out, f) <= max_w) return;
  const int ell = text_width("\xE2\x80\xA6", f);
  size_t len = strlen(out);
  while (len > 0) {
    do { len--; } while (len > 0 && (out[len] & 0xC0) == 0x80);  // whole code points only
    char tmp[96];
    size_t k = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
    memcpy(tmp, out, k);
    while (k > 0 && tmp[k - 1] == ' ') k--;
    tmp[k] = '\0';
    if (text_width(tmp, f) + ell <= max_w) {
      snprintf(out, n, "%s\xE2\x80\xA6", tmp);
      return;
    }
  }
  snprintf(out, n, "\xE2\x80\xA6");
}

int OledGfx::text(const char *s, int x, int y, const lv_font_t *f, int lv, TextAlign a, int max_w) {
  if (!s || !*s || !f) return 0;
  char buf[96];
  fit(s, f, max_w, buf, sizeof(buf));
  const int w = text_width(buf, f);
  int pen = a == TextAlign::Right ? x - w : (a == TextAlign::Center ? x - w / 2 : x);
  const char *p = buf;
  uint32_t c;
  while ((c = utf8_next(&p)) != 0) {
    int adv = 0;
    glyph(f, c, utf8_peek(p), pen, y, lv, &adv);
    pen += adv;
  }
  return w;
}

// ---- icons (the prototype's, on a 12 px grid) ----

void OledGfx::icon(const char *n, int x, int y, int lv) {
  if (!strcmp(n, "note")) {
    line(x + 4, y + 9, x + 4, y + 1, lv);
    line(x + 4, y + 1, x + 10, y, lv);
    line(x + 10, y, x + 10, y + 8, lv);
    disc(x + 2.6f, y + 9.5f, 1.8f, lv);
    disc(x + 8.6f, y + 8.5f, 1.8f, lv);
  } else if (!strcmp(n, "play")) {
    tri(x + 2, y + 1, x + 10, y + 6, x + 2, y + 11, lv);
  } else if (!strcmp(n, "pause")) {
    box(x + 2, y + 1, 3, 10, lv);
    box(x + 7, y + 1, 3, 10, lv);
  } else if (!strcmp(n, "warn")) {
    tri(x + 6, y, x + 12, y + 11, x, y + 11, lv);
    box(x + 5, y + 4, 2, 4, 0);
    box(x + 5, y + 9, 2, 1, 0);
  } else if (!strcmp(n, "gps")) {
    ring(x + 6, y + 5, 3.6f, lv);
    line(x + 3, y + 7.2f, x + 6, y + 11.5f, lv);
    line(x + 9, y + 7.2f, x + 6, y + 11.5f, lv);
    disc(x + 6, y + 5, 1.2f, lv);
  } else if (!strcmp(n, "bt")) {
    line(x + 3, y + 3.5f, x + 9, y + 8.5f, lv);
    line(x + 9, y + 8.5f, x + 6, y + 11, lv);
    line(x + 6, y + 11, x + 6, y + 1, lv);
    line(x + 6, y + 1, x + 9, y + 3.5f, lv);
    line(x + 9, y + 3.5f, x + 3, y + 8.5f, lv);
  } else if (!strcmp(n, "spk")) {
    box(x + 1, y + 4, 3, 4, lv);
    tri(x + 4, y + 4, x + 8, y + 1, x + 8, y + 11, lv);
    tri(x + 4, y + 4, x + 8, y + 11, x + 4, y + 8, lv);
  } else if (!strcmp(n, "ext")) {
    line(x + 3, y + 9, x + 10, y + 2, lv);
    line(x + 5, y + 2, x + 10, y + 2, lv);
    line(x + 10, y + 2, x + 10, y + 7, lv);
  } else if (!strcmp(n, "round")) {
    ring(x + 6, y + 6, 5.2f, lv);
    disc(x + 6, y + 6, 1.6f, lv);
  } else if (!strcmp(n, "phone")) {  // a handset, for the call screens (not in the prototype, drawn to match)
    line(x + 2.5f, y + 2, x + 5, y + 1.5f, lv, 2.2f);
    line(x + 2.5f, y + 2, x + 2, y + 5, lv, 2.2f);
    line(x + 2, y + 5, x + 7, y + 10, lv, 1.6f);
    line(x + 7, y + 10, x + 10, y + 9.5f, lv, 2.2f);
    line(x + 10, y + 9.5f, x + 10.5f, y + 7, lv, 2.2f);
  }
}

void OledGfx::poly(const float (*p)[2], int n, float ox, float oy, int lv) {
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (int i = 0; i < n; i++) {
    x0 = fminf(x0, p[i][0] + ox);
    x1 = fmaxf(x1, p[i][0] + ox);
    y0 = fminf(y0, p[i][1] + oy);
    y1 = fmaxf(y1, p[i][1] + oy);
  }
  for (int j = (int)floorf(y0); j <= (int)ceilf(y1); j++) {
    for (int i = (int)floorf(x0); i <= (int)ceilf(x1); i++) {
      int in = 0;  // 4×4 samples, even-odd rule
      for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
          const float px = i + (sx + 0.5f) / 4 - ox, py = j + (sy + 0.5f) / 4 - oy;
          bool odd = false;
          for (int a = 0, b = n - 1; a < n; b = a++) {
            if ((p[a][1] > py) != (p[b][1] > py) &&
                px < (p[b][0] - p[a][0]) * (py - p[a][1]) / (p[b][1] - p[a][1]) + p[a][0]) {
              odd = !odd;
            }
          }
          in += odd;
        }
      }
      if (in) plot(i, j, lv, (in * 15 + 8) / 16);
    }
  }
}

void OledGfx::bowtie(int lv, int fill) {
  // The bowtie, symmetric under a half turn, with the reference outline's proportions (height 248 units: body 354
  // wide, wings 156 tall reaching 858 across, ends slanting 127 over the wing height), scaled to 38 px tall.
  const float k = 38.0f / 248.0f;
  const float bw = 177 * k, bh = 124 * k, wh = 78 * k, tip = 429 * k, slant = 127 * k;
  const float P[12][2] = {{-bw, -bh}, {bw, -bh}, {bw, -wh}, {tip, -wh},  {tip - slant, wh},   {bw, wh},
                          {bw, bh},   {-bw, bh}, {-bw, wh}, {-tip, wh}, {-tip + slant, -wh}, {-bw, -wh}};
  const float cx = 128, cy = 29;
  if (fill > 0) poly(P, 12, cx, cy, fill);
  for (int i = 0; i < 12; i++) {
    const float *a = P[i], *b = P[(i + 1) % 12];
    line(cx + a[0], cy + a[1], cx + b[0], cy + b[1], lv, 2.6f);
  }
}

void OledGfx::sheen(float pos, float width, float gain, float lift) {
  // A diagonal band (leaning like the bowtie's ends) over what is already lit: bright parts brighten by `gain`, and
  // everything lit (the faint fill too) gains `lift` levels, so the band reads across the middle as well.
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      uint8_t &p = fb_[y * W + x];
      if (!p) continue;
      const float d = (x - pos) + (y - 29) * 0.7f;
      const float k = expf(-(d * d) / (width * width));
      const int v = (int)lroundf(p * (1.0f + gain * k) + lift * k);
      p = (uint8_t)(v > 15 ? 15 : v);
    }
  }
}
