#include "tsps_cluster.h"

#include <cmath>
#include <cstring>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

#include "tsps_assets_gen.h"
#include "tsps_classic1_assets_gen.h"
#include <sensor_packet.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int LAYOUT_REF = 1080;
static const uint16_t BG_565 = 0x2104;  // ~#121212

static uint16_t rgb888_to_565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static uint16_t needle_color() { return rgb888_to_565(255, 52, 52); }

static float map_angle(float ratio, float angle_start_deg, float angle_end_deg) {
  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }
  float start = fmodf(angle_start_deg, 360.0f);
  if (start < 0.0f) {
    start += 360.0f;
  }
  float end = fmodf(angle_end_deg, 360.0f);
  if (end < 0.0f) {
    end += 360.0f;
  }
  float span = fmodf(end - start, 360.0f);
  if (span < 0.0f) {
    span += 360.0f;
  }
  float out = start + span * ratio;
  out = fmodf(out, 360.0f);
  if (out < 0.0f) {
    out += 360.0f;
  }
  return out;
}

static float user_to_math_deg(float user_angle_deg) { return fmodf(270.0f - user_angle_deg, 360.0f); }

static void plot4(uint16_t *fb, int w, int h, int x, int y, uint16_t c) {
  if (x < 0 || y < 0 || x >= w || y >= h) {
    return;
  }
  fb[y * w + x] = c;
}

static void draw_line_thick(uint16_t *fb, int w, int h, int x0, int y0, int x1, int y1, uint16_t c, int thick) {
  const int steps = 500;
  for (int i = 0; i <= steps; i++) {
    float t = (float)i / (float)steps;
    int x = x0 + (int)roundf((float)(x1 - x0) * t);
    int y = y0 + (int)roundf((float)(y1 - y0) * t);
    for (int ox = -thick; ox <= thick; ox++) {
      for (int oy = -thick; oy <= thick; oy++) {
        if (ox * ox + oy * oy <= thick * thick + thick) {
          plot4(fb, w, h, x + ox, y + oy, c);
        }
      }
    }
  }
}

static void blend_src_over(uint16_t *dst, const uint16_t *src565, const uint8_t *alpha, int n) {
  for (int i = 0; i < n; i++) {
    uint8_t a = alpha[i];
    if (a < 4) {
      continue;
    }
    uint16_t s = src565[i];
    if (a > 250) {
      dst[i] = s;
      continue;
    }
    uint16_t d = dst[i];
    uint32_t sr = (s >> 11) & 0x1F;
    uint32_t sg = (s >> 5) & 0x3F;
    uint32_t sb = s & 0x1F;
    uint32_t dr = (d >> 11) & 0x1F;
    uint32_t dg = (d >> 5) & 0x3F;
    uint32_t db = d & 0x1F;
    uint32_t ia = 255 - a;
    uint32_t r = (sr * a + dr * ia) / 255;
    uint32_t g = (sg * a + dg * ia) / 255;
    uint32_t b = (sb * a + db * ia) / 255;
    dst[i] = (uint16_t)((r << 11) | (g << 5) | b);
  }
}

static void blend_overlay_shift(uint16_t *fb, int cw, const uint16_t *ovl565, const uint8_t *ovlA, int ox, int oy) {
  for (int y = 0; y < cw; y++) {
    for (int x = 0; x < cw; x++) {
      int di = y * cw + x;
      int tx = x + ox;
      int ty = y + oy;
      if (tx < 0 || ty < 0 || tx >= cw || ty >= cw) {
        continue;
      }
      int ti = ty * cw + tx;
      uint8_t a = ovlA[ti];
      if (a < 4) {
        continue;
      }
      uint16_t s = ovl565[ti];
      uint16_t d = fb[di];
      if (a > 250) {
        fb[di] = s;
        continue;
      }
      uint32_t sr = (s >> 11) & 0x1F;
      uint32_t sg = (s >> 5) & 0x3F;
      uint32_t sb = s & 0x1F;
      uint32_t dr = (d >> 11) & 0x1F;
      uint32_t dg = (d >> 5) & 0x3F;
      uint32_t db = d & 0x1F;
      uint32_t ia = 255 - a;
      uint32_t r = (sr * a + dr * ia) / 255;
      uint32_t g = (sg * a + dg * ia) / 255;
      uint32_t b = (sb * a + db * ia) / 255;
      fb[di] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
}

struct SubGauge {
  float v0, v1;
  float a0, a1;
  int ox, oy;
  float len_ratio;
  int line_w;
  bool invert;
};

static void draw_sub(const SubGauge &g, float value, float layout_scale, uint16_t *fb, int cw) {
  float ratio = (g.v1 <= g.v0) ? 0.0f : (value - g.v0) / (g.v1 - g.v0);
  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }
  if (g.invert) {
    ratio = 1.0f - ratio;
  }
  float ang = map_angle(ratio, g.a0, g.a1);
  float mdeg = user_to_math_deg(ang) * (float)M_PI / 180.0f;
  int cx = (int)(0.5f * LAYOUT_REF * layout_scale) + (int)((float)g.ox * layout_scale);
  int cy = (int)(0.42f * LAYOUT_REF * layout_scale) + (int)((float)g.oy * layout_scale);
  int len = (int)((float)LAYOUT_REF * layout_scale * g.len_ratio);
  int tipx = cx + (int)(cosf(mdeg) * (float)len);
  int tipy = cy - (int)(sinf(mdeg) * (float)len);
  int tw = (int)ceilf((float)g.line_w * layout_scale / 3.0f);
  if (tw < 1) {
    tw = 1;
  }
  if (tw > 5) {
    tw = 5;
  }
  draw_line_thick(fb, cw, cw, cx, cy, tipx, tipy, needle_color(), tw);
}

static void scale_blit_to_screen(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, uint16_t *fb, int cw) {
  const int out_s = (screen_w < screen_h) ? (int)screen_w : (int)screen_h;
  uint16_t *blit_buf = fb;
  int blit_w = cw;
  int blit_h = cw;

  if (out_s != cw && out_s > 0) {
    static uint16_t *scaled_fb = nullptr;
    static size_t scaled_cap_pixels = 0;
    const size_t need = (size_t)out_s * (size_t)out_s;
    if (need > scaled_cap_pixels || scaled_fb == nullptr) {
#if defined(ESP_PLATFORM)
      free(scaled_fb);
      scaled_fb = (uint16_t *)heap_caps_malloc(need * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
      free(scaled_fb);
      scaled_fb = (uint16_t *)malloc(need * sizeof(uint16_t));
#endif
      scaled_cap_pixels = scaled_fb ? need : 0;
    }
    if (scaled_fb) {
      for (int yo = 0; yo < out_s; yo++) {
        const int yi = (yo * cw) / out_s;
        const int yc = (yi < cw) ? yi : (cw - 1);
        for (int xo = 0; xo < out_s; xo++) {
          const int xi = (xo * cw) / out_s;
          const int xc = (xi < cw) ? xi : (cw - 1);
          scaled_fb[yo * out_s + xo] = fb[yc * cw + xc];
        }
      }
      blit_buf = scaled_fb;
      blit_w = blit_h = out_s;
    }
  }

  const float blit_to_ref = (float)blit_w / (float)LAYOUT_REF;
  int16_t dx = (int16_t)((screen_w - blit_w) / 2);
  int16_t dy = (int16_t)((screen_h - blit_h) / 2);
  int16_t dy2 = dy + (int16_t)roundf(-11.0f * blit_to_ref);
  gfx->draw16bitRGBBitmap(dx, dy2, blit_buf, blit_w, blit_h);
  gfx->flush(true);
}

static void tsps_render_classic2(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, const SensorDerived &s) {
  const int cw = TSPS_ASSET_EDGE;
  static uint16_t *fb = nullptr;
  if (!fb) {
#if defined(ESP_PLATFORM)
    fb = (uint16_t *)heap_caps_malloc(cw * cw * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!fb) {
      fb = (uint16_t *)malloc(cw * cw * sizeof(uint16_t));
    }
  }
  if (!fb) {
    return;
  }

  float layout_scale = (float)cw / (float)LAYOUT_REF;

  for (int i = 0; i < cw * cw; i++) {
    fb[i] = BG_565;
  }

  float oil = s.oil_psi;
  float fuel = s.fuel_pct;
  float temp = s.coolant_temp_f;
  float amp = s.system_amp;

  SubGauge oil_g = {0, 60, 250, 290, 0, 40, 0.41f, 10, false};
  SubGauge fuel_g = {0, 100, 152, 208, 0, 0, 0.35f, 10, false};
  SubGauge temp_g = {40, 250, -20, 20, 0, 50, 0.4f, 10, true};
  SubGauge amp_g = {-100, 100, 68, 110, 0, 40, 0.41f, 10, false};

  draw_sub(oil_g, oil, layout_scale, fb, cw);
  draw_sub(fuel_g, fuel, layout_scale, fb, cw);
  draw_sub(temp_g, temp, layout_scale, fb, cw);
  draw_sub(amp_g, amp, layout_scale, fb, cw);

  size_t px = (size_t)cw * (size_t)cw;
  static uint16_t *dial565 = nullptr;
  static uint8_t *dialA = nullptr;
  static uint16_t *ovl565 = nullptr;
  static uint8_t *ovlA = nullptr;
  if (!dial565) {
#if defined(ESP_PLATFORM)
    dial565 = (uint16_t *)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    dialA = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ovl565 = (uint16_t *)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ovlA = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    dial565 = (uint16_t *)malloc(px * sizeof(uint16_t));
    dialA = (uint8_t *)malloc(px);
    ovl565 = (uint16_t *)malloc(px * sizeof(uint16_t));
    ovlA = (uint8_t *)malloc(px);
#endif
    if (dial565 && dialA && ovl565 && ovlA) {
      memcpy(dial565, tsps_dial_rgb565, px * sizeof(uint16_t));
      memcpy(dialA, tsps_dial_alpha, px);
      memcpy(ovl565, tsps_overlay_rgb565, px * sizeof(uint16_t));
      memcpy(ovlA, tsps_overlay_alpha, px);
    }
  }
  if (dial565 && dialA) {
    blend_src_over(fb, dial565, dialA, (int)px);
  }
  if (ovl565 && ovlA) {
    int ox = (int)roundf(5.0f * layout_scale);
    int oy = (int)roundf(6.0f * layout_scale);
    blend_overlay_shift(fb, cw, ovl565, ovlA, ox, oy);
  }

  scale_blit_to_screen(gfx, screen_w, screen_h, fb, cw);
}

static void tsps_render_classic1(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, const SensorDerived &s) {
  const int cw = TSPS_C1_ASSET_EDGE;
  static uint16_t *fb = nullptr;
  if (!fb) {
#if defined(ESP_PLATFORM)
    fb = (uint16_t *)heap_caps_malloc(cw * cw * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!fb) {
      fb = (uint16_t *)malloc(cw * cw * sizeof(uint16_t));
    }
  }
  if (!fb) {
    return;
  }

  float layout_scale = (float)cw / (float)LAYOUT_REF;

  for (int i = 0; i < cw * cw; i++) {
    fb[i] = BG_565;
  }

  SubGauge fuel_g = {0, 100, 145, 215, 0, 0, 0.2f, 10, false};
  draw_sub(fuel_g, s.fuel_pct, layout_scale, fb, cw);

  size_t px = (size_t)cw * (size_t)cw;
  static uint16_t *dial565 = nullptr;
  static uint8_t *dialA = nullptr;
  static uint16_t *ovl565 = nullptr;
  static uint8_t *ovlA = nullptr;
  if (!dial565) {
#if defined(ESP_PLATFORM)
    dial565 = (uint16_t *)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    dialA = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ovl565 = (uint16_t *)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ovlA = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    dial565 = (uint16_t *)malloc(px * sizeof(uint16_t));
    dialA = (uint8_t *)malloc(px);
    ovl565 = (uint16_t *)malloc(px * sizeof(uint16_t));
    ovlA = (uint8_t *)malloc(px);
#endif
    if (dial565 && dialA && ovl565 && ovlA) {
      memcpy(dial565, tsps_c1_dial_rgb565, px * sizeof(uint16_t));
      memcpy(dialA, tsps_c1_dial_alpha, px);
      memcpy(ovl565, tsps_c1_overlay_rgb565, px * sizeof(uint16_t));
      memcpy(ovlA, tsps_c1_overlay_alpha, px);
    }
  }
  if (dial565 && dialA) {
    blend_src_over(fb, dial565, dialA, (int)px);
  }
  if (ovl565 && ovlA) {
    int ox = 0;
    int oy = (int)roundf(-5.0f * layout_scale);
    blend_overlay_shift(fb, cw, ovl565, ovlA, ox, oy);
  }

  scale_blit_to_screen(gfx, screen_w, screen_h, fb, cw);
}

void tsps_render_cluster(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, const SensorDerived &s, bool classic2) {
  if (classic2) {
    tsps_render_classic2(gfx, screen_w, screen_h, s);
  } else {
    tsps_render_classic1(gfx, screen_w, screen_h, s);
  }
}
