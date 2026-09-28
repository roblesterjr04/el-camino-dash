// The painter runs per pixel: always build it optimised, even in -Os firmware builds.
#pragma GCC optimize("O3", "fast-math")

#include "paint.h"

#include <string.h>

namespace gauge {

namespace {

struct Glyph {
  lv_font_glyph_dsc_t dsc;
  const uint8_t *bmp;
};

bool glyph(const lv_font_t *font, uint32_t letter, uint32_t next, Glyph *g) {
  if (!lv_font_get_glyph_dsc(font, &g->dsc, letter, next)) {
    return false;
  }
  g->bmp = (g->dsc.box_w && g->dsc.box_h) ? lv_font_get_glyph_bitmap(g->dsc.resolved_font ? g->dsc.resolved_font : font, letter)
                                          : nullptr;
  return true;
}

/** Glyph coverage 0..1 at integer bitmap position (continuous bit packing, MSB first). */
inline float glyph_px(const Glyph &g, int col, int row) {
  if (col < 0 || row < 0 || col >= g.dsc.box_w || row >= g.dsc.box_h) {
    return 0.0f;
  }
  const uint8_t bpp = g.dsc.bpp;
  const uint32_t bit = ((uint32_t)row * g.dsc.box_w + (uint32_t)col) * bpp;
  const uint8_t byte = g.bmp[bit >> 3];
  const uint8_t mask = (uint8_t)((1u << bpp) - 1u);
  const uint8_t v = (uint8_t)((byte >> (8 - bpp - (bit & 7))) & mask);
  return (float)v / (float)mask;
}

inline float glyph_bilinear(const Glyph &g, float u, float v) {
  const float fu = floorf(u), fv = floorf(v);
  const int c = (int)fu, r = (int)fv;
  const float tu = u - fu, tv = v - fv;
  const float a = glyph_px(g, c, r), b = glyph_px(g, c + 1, r);
  const float d = glyph_px(g, c, r + 1), e = glyph_px(g, c + 1, r + 1);
  return lerpf(lerpf(a, b, tu), lerpf(d, e, tu), tv);
}

/**
 * Draw one glyph whose box top-left sits at local (lx, ly) in a frame anchored at (ax, ay) and rotated
 * by angle (cos ca, sin sa).
 */
void draw_glyph(Canvas888 &s, const Glyph &g, float lx, float ly, float ax, float ay, float ca, float sa, Rgb c,
                float opa) {
  if (!g.bmp) {
    return;
  }
  const float bw = (float)g.dsc.box_w, bh = (float)g.dsc.box_h;
  if (sa == 0.0f && ca == 1.0f) {
    // Upright: snap to whole pixels so small text stays crisp.
    const int x0 = (int)lroundf(ax + lx), y0 = (int)lroundf(ay + ly);
    for (int r = 0; r < g.dsc.box_h; r++) {
      const int y = y0 + r;
      if (y < s.clip_y0 || y >= s.clip_y1) {
        continue;
      }
      for (int col = 0; col < g.dsc.box_w; col++) {
        const int x = x0 + col;
        if (x < s.clip_x0 || x >= s.clip_x1) {
          continue;
        }
        const float a = glyph_px(g, col, r);
        if (a > 0.0f) {
          s.blend(x, y, c, a * opa);
        }
      }
    }
    return;
  }
  // Rotated: sample the glyph bilinearly over the rotated box.
  float xs[4], ys[4];
  const float cu[4] = {lx, lx + bw, lx, lx + bw}, cv[4] = {ly, ly, ly + bh, ly + bh};
  for (int i = 0; i < 4; i++) {
    xs[i] = ax + cu[i] * ca - cv[i] * sa;
    ys[i] = ay + cu[i] * sa + cv[i] * ca;
  }
  Box b{fminf(fminf(xs[0], xs[1]), fminf(xs[2], xs[3])), fminf(fminf(ys[0], ys[1]), fminf(ys[2], ys[3])),
        fmaxf(fmaxf(xs[0], xs[1]), fmaxf(xs[2], xs[3])), fmaxf(fmaxf(ys[0], ys[1]), fmaxf(ys[2], ys[3]))};
  fill(s, box_grow(b, 1), [&](float x, float y, Rgb &o) {
    const float dx = x - ax, dy = y - ay;
    const float u = dx * ca + dy * sa - lx - 0.5f;
    const float v = -dx * sa + dy * ca - ly - 0.5f;
    o = c;
    return glyph_bilinear(g, u, v) * opa;
  });
}

}  // namespace

float text_width(const lv_font_t *font, const char *s) {
  float w = 0.0f;
  uint32_t i = 0;
  while (s[i]) {
    const uint32_t letter = _lv_txt_encoded_next(s, &i);
    uint32_t j = i;
    const uint32_t next = s[j] ? _lv_txt_encoded_next(s, &j) : 0;
    w += (float)lv_font_get_glyph_width(font, letter, next);
  }
  return w;
}

void text(Canvas888 &s, const lv_font_t *font, const char *str, float x, float y, Align align, Rgb c, float angle,
          float opa) {
  const float w = text_width(font, str);
  float pen = align == Align::Left ? 0.0f : (align == Align::Center ? -w * 0.5f : -w);
  // Local frame: origin at the anchor; baseline below the middle of the line box.
  const float baseline = -(float)font->line_height * 0.5f + (float)(font->line_height - font->base_line);
  const float ca = angle == 0.0f ? 1.0f : cosf(rad(angle));
  const float sa = angle == 0.0f ? 0.0f : sinf(rad(angle));
  uint32_t i = 0;
  while (str[i]) {
    const uint32_t letter = _lv_txt_encoded_next(str, &i);
    uint32_t j = i;
    const uint32_t next = str[j] ? _lv_txt_encoded_next(str, &j) : 0;
    Glyph g;
    if (!glyph(font, letter, next, &g)) {
      continue;
    }
    const float lx = pen + (float)g.dsc.ofs_x;
    const float ly = baseline - (float)(g.dsc.box_h + g.dsc.ofs_y);
    draw_glyph(s, g, lx, ly, x, y, ca, sa, c, opa);
    pen += (float)g.dsc.adv_w;
  }
}

void text_arc(Canvas888 &s, const lv_font_t *font, const char *str, float cx, float cy, float r, float psi,
              bool top, Rgb c, float letter_spacing) {
  // Total advance including spacing.
  int n = 0;
  float w = 0.0f;
  {
    uint32_t i = 0;
    while (str[i]) {
      const uint32_t letter = _lv_txt_encoded_next(str, &i);
      uint32_t j = i;
      const uint32_t next = str[j] ? _lv_txt_encoded_next(str, &j) : 0;
      w += (float)lv_font_get_glyph_width(font, letter, next);
      n++;
    }
  }
  w += letter_spacing * (float)(n > 0 ? n - 1 : 0);
  const float deg_per_px = 180.0f / (kPi * r);
  float pen = 0.0f;
  uint32_t i = 0;
  while (str[i]) {
    const uint32_t start = i;
    const uint32_t letter = _lv_txt_encoded_next(str, &i);
    uint32_t j = i;
    const uint32_t next = str[j] ? _lv_txt_encoded_next(str, &j) : 0;
    const float adv = (float)lv_font_get_glyph_width(font, letter, next);
    const float mid = pen + adv * 0.5f - w * 0.5f;  // glyph centre relative to text centre (px along the arc)
    char one[8] = {0};
    memcpy(one, str + start, i - start < 7 ? i - start : 7);
    if (top) {
      const float pg = psi + mid * deg_per_px;
      text(s, font, one, polar_x(cx, r, pg), polar_y(cy, r, pg), Align::Center, c, pg);
    } else {
      const float pg = psi - mid * deg_per_px;
      text(s, font, one, polar_x(cx, r, pg), polar_y(cy, r, pg), Align::Center, c, pg - 180.0f);
    }
    pen += adv + letter_spacing;
  }
}

namespace {
const uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

inline uint16_t pack565(int x, int y, const uint8_t *p) {
  const float d = ((float)kBayer[y & 3][x & 3] + 0.5f) / 16.0f;  // 0..1
  const int r = (int)clampf((float)p[0] + (d - 0.5f) * 8.0f, 0, 255) >> 3;
  const int g = (int)clampf((float)p[1] + (d - 0.5f) * 4.0f, 0, 255) >> 2;
  const int b = (int)clampf((float)p[2] + (d - 0.5f) * 8.0f, 0, 255) >> 3;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
}  // namespace

void to_rgb565(const Canvas888 &src, uint16_t *dst) {
  const int bpp = src.alpha ? 4 : 3;
  for (int y = 0; y < src.h; y++) {
    const uint8_t *p = src.px + (size_t)y * (size_t)src.w * bpp;
    uint16_t *d = dst + (size_t)y * (size_t)src.w;
    for (int x = 0; x < src.w; x++, p += bpp) {
      d[x] = pack565(x, y, p);
    }
  }
}

void to_rgb565a8(const Canvas888 &src, uint8_t *dst) {
  for (int y = 0; y < src.h; y++) {
    const uint8_t *p = src.px + (size_t)y * (size_t)src.w * 4;
    uint8_t *d = dst + (size_t)y * (size_t)src.w * 3;
    for (int x = 0; x < src.w; x++, p += 4, d += 3) {
      const uint16_t c = pack565(x, y, p);
      d[0] = (uint8_t)(c & 0xFF);
      d[1] = (uint8_t)(c >> 8);
      d[2] = p[3];
    }
  }
}

}  // namespace gauge
