#include "speedo_cluster.h"

#include <cmath>
#include <cstring>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

#include "speedo_assets_gen.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int LAYOUT_REF = 1080;
static const uint16_t BG_565 = 0x2104;

static float map_angle(float ratio, float angle_start_deg, float angle_end_deg) {
  ratio = fmaxf(0.0f, fminf(1.0f, ratio));
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
  float out = fmodf(start + span * ratio, 360.0f);
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
  for (int i = 0; i <= 400; i++) {
    float t = (float)i / 400.0f;
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

static uint16_t needle565() {
  return (uint16_t)(((31 << 11) | (10 << 5) | 8));  // ~red needle
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
    dst[i] = (uint16_t)((((sr * a + dr * ia) / 255) << 11) | (((sg * a + dg * ia) / 255) << 5) | ((sb * a + db * ia) / 255));
  }
}

struct SubGauge {
  float v0, v1;
  float a0, a1;
  int ox, oy;
  float len_ratio;
  int line_w;
};

static void draw_speed_needle(const SubGauge &g, float mph, float layout_scale, uint16_t *fb, int cw) {
  float ratio = (g.v1 <= g.v0) ? 0.0f : (mph - g.v0) / (g.v1 - g.v0);
  ratio = fmaxf(0.0f, fminf(1.0f, ratio));
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
  if (tw > 6) {
    tw = 6;
  }
  draw_line_thick(fb, cw, cw, cx, cy, tipx, tipy, needle565(), tw);
}

static void scale_blit(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, uint16_t *fb, int cw) {
  const int out_s = (screen_w < screen_h) ? (int)screen_w : (int)screen_h;
  uint16_t *blit_buf = fb;
  int blit_w = cw;
  if (out_s != cw && out_s > 0) {
    static uint16_t *scaled = nullptr;
    static size_t cap = 0;
    size_t need = (size_t)out_s * (size_t)out_s;
    if (need > cap || !scaled) {
      free(scaled);
#if defined(ESP_PLATFORM)
      scaled = (uint16_t *)heap_caps_malloc(need * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
      scaled = (uint16_t *)malloc(need * sizeof(uint16_t));
#endif
      cap = scaled ? need : 0;
    }
    if (scaled) {
      for (int yo = 0; yo < out_s; yo++) {
        int yi = (yo * cw) / out_s;
        if (yi >= cw) {
          yi = cw - 1;
        }
        for (int xo = 0; xo < out_s; xo++) {
          int xi = (xo * cw) / out_s;
          if (xi >= cw) {
            xi = cw - 1;
          }
          scaled[yo * out_s + xo] = fb[yi * cw + xi];
        }
      }
      blit_buf = scaled;
      blit_w = out_s;
    }
  }
  int16_t dx = (int16_t)((screen_w - blit_w) / 2);
  int16_t dy = (int16_t)((screen_h - blit_w) / 2);
  gfx->draw16bitRGBBitmap(dx, dy, blit_buf, blit_w, blit_w);
  gfx->flush(true);
}

void speedo_render(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, float mph) {
  const int cw = SPEEDO_ASSET_EDGE;
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
  float ls = (float)cw / (float)LAYOUT_REF;
  for (int i = 0; i < cw * cw; i++) {
    fb[i] = BG_565;
  }

  SubGauge sg = {0.0f, 140.0f, 60.0f, 300.0f, 0, -5, 0.36f, 12};
  draw_speed_needle(sg, mph, ls, fb, cw);

  size_t px = (size_t)cw * (size_t)cw;
  static uint16_t *d565 = nullptr;
  static uint8_t *dA = nullptr;
  static uint16_t *o565 = nullptr;
  static uint8_t *oA = nullptr;
  if (!d565) {
#if defined(ESP_PLATFORM)
    d565 = (uint16_t *)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    dA = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    o565 = (uint16_t *)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    oA = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    d565 = (uint16_t *)malloc(px * sizeof(uint16_t));
    dA = (uint8_t *)malloc(px);
    o565 = (uint16_t *)malloc(px * sizeof(uint16_t));
    oA = (uint8_t *)malloc(px);
#endif
    if (d565 && dA && o565 && oA) {
      memcpy(d565, speedo_dial_rgb565, px * sizeof(uint16_t));
      memcpy(dA, speedo_dial_alpha, px);
      memcpy(o565, speedo_overlay_rgb565, px * sizeof(uint16_t));
      memcpy(oA, speedo_overlay_alpha, px);
    }
  }
  if (d565 && dA) {
    blend_src_over(fb, d565, dA, (int)px);
  }
  if (o565 && oA) {
    for (int y = 0; y < cw; y++) {
      for (int x = 0; x < cw; x++) {
        int di = y * cw + x;
        uint8_t a = oA[di];
        if (a < 4) {
          continue;
        }
        uint16_t s = o565[di];
        uint16_t d = fb[di];
        if (a > 250) {
          fb[di] = s;
          continue;
        }
        uint32_t sr = (s >> 11) & 0x1F, sg = (s >> 5) & 0x3F, sb = s & 0x1F;
        uint32_t dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
        uint32_t ia = 255 - a;
        fb[di] = (uint16_t)((((sr * a + dr * ia) / 255) << 11) | (((sg * a + dg * ia) / 255) << 5) | ((sb * a + db * ia) / 255));
      }
    }
  }

  scale_blit(gfx, screen_w, screen_h, fb, cw);
}
