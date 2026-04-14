#include "center_cluster.h"

#include "center_audio_link.h"
#include "center_fallout.h"

#include <cmath>
#include <cstring>
#include <stdio.h>
#include <time.h>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include <pgmspace.h>
#endif

#include "center_assets_gen.h"
#include "center_clock_assets_gen.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int LAYOUT_REF = 1080;
static const uint16_t BG_565 = 0x0000;

// classic_2 theme.json clock_render._default
static const float kClockPivotX = 0.5f;
static const float kClockPivotY = 0.9f;
static const float kAngleOffsetDeg = -90.0f;

static float map_angle(float ratio, float a0, float a1) {
  ratio = fmaxf(0.0f, fminf(1.0f, ratio));
  float start = fmodf(a0, 360.0f);
  if (start < 0.0f) {
    start += 360.0f;
  }
  float end = fmodf(a1, 360.0f);
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

static float user_to_math_deg(float u) { return fmodf(270.0f - u, 360.0f); }

static void plot4(uint16_t *fb, int w, int h, int x, int y, uint16_t c) {
  if (x >= 0 && y >= 0 && x < w && y < h) {
    fb[y * w + x] = c;
  }
}

static void draw_line_thick(uint16_t *fb, int w, int h, int x0, int y0, int x1, int y1, uint16_t c, int thick) {
  for (int i = 0; i <= 300; i++) {
    float t = (float)i / 300.0f;
    int x = x0 + (int)roundf((x1 - x0) * t);
    int y = y0 + (int)roundf((y1 - y0) * t);
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
    uint32_t sr = (s >> 11) & 0x1F, sg = (s >> 5) & 0x3F, sb = s & 0x1F;
    uint32_t dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    uint32_t ia = 255 - a;
    dst[i] = (uint16_t)((((sr * a + dr * ia) / 255) << 11) | (((sg * a + dg * ia) / 255) << 5) | ((sb * a + db * ia) / 255));
  }
}

// src-over blend for a sw×sh sprite placed at (ox, oy) on a dst_stride-wide buffer.
static void blend_src_over_rect(uint16_t *dst, int dst_stride, const uint16_t *src565, const uint8_t *alpha, int sw, int sh,
                                int ox, int oy) {
  for (int iy = 0; iy < sh; iy++) {
    for (int ix = 0; ix < sw; ix++) {
      int si = iy * sw + ix;
      uint8_t a = alpha[si];
      if (a < 4) {
        continue;
      }
      int dx = ox + ix;
      int dy = oy + iy;
      if (dx < 0 || dy < 0 || dx >= dst_stride || dy >= dst_stride) {
        continue;
      }
      uint16_t s = src565[si];
      uint16_t *dp = &dst[dy * dst_stride + dx];
      if (a > 250) {
        *dp = s;
        continue;
      }
      uint16_t d = *dp;
      uint32_t sr = (s >> 11) & 0x1F, sg = (s >> 5) & 0x3F, sb = s & 0x1F;
      uint32_t dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
      uint32_t ia = 255 - a;
      *dp = (uint16_t)((((sr * a + dr * ia) / 255) << 11) | (((sg * a + dg * ia) / 255) << 5) | ((sb * a + db * ia) / 255));
    }
  }
}

static void blend_pixel_over(uint16_t *dst, uint16_t s, uint8_t a) {
  if (a < 4) {
    return;
  }
  if (a > 250) {
    *dst = s;
    return;
  }
  uint16_t d = *dst;
  uint32_t sr = (s >> 11) & 0x1F, sg = (s >> 5) & 0x3F, sb = s & 0x1F;
  uint32_t dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
  uint32_t ia = 255 - a;
  *dst = (uint16_t)((((sr * a + dr * ia) / 255) << 11) | (((sg * a + dg * ia) / 255) << 5) | ((sb * a + db * ia) / 255));
}

#if defined(ESP_PLATFORM)
static uint16_t rd565(const uint16_t *p, int i) { return (uint16_t)pgm_read_word(p + i); }
static uint8_t rdA(const uint8_t *p, int i) { return pgm_read_byte(p + i); }
#else
static uint16_t rd565(const uint16_t *p, int i) { return p[i]; }
static uint8_t rdA(const uint8_t *p, int i) { return p[i]; }
#endif

// Rotated sprite: matches gauge_screen._draw_clock_sprite_hand (math angle + angle_offset).
static void blend_rotated_needle(uint16_t *fb, int cw, const uint16_t *src565, const uint8_t *alpha, int sw, int sh,
                                 float user_angle_deg) {
  const float px = kClockPivotX * (float)sw;
  const float py = kClockPivotY * (float)sh;
  const float cx = 0.5f * (float)cw;
  const float cy = 0.5f * (float)cw;
  const float deg = user_to_math_deg(user_angle_deg) + kAngleOffsetDeg;
  const float th = deg * (float)M_PI / 180.0f;
  const float c = cosf(th);
  const float s = sinf(th);
  for (int iy = 0; iy < sh; iy++) {
    for (int ix = 0; ix < sw; ix++) {
      const int idx = iy * sw + ix;
      const uint8_t a = rdA(alpha, idx);
      if (a < 4) {
        continue;
      }
      const uint16_t col = rd565(src565, idx);
      const float dx = (float)ix - px;
      const float dy = (float)iy - py;
      const int wx = (int)roundf(cx + dx * c - dy * s);
      const int wy = (int)roundf(cy - (dx * s + dy * c));
      if (wx < 0 || wy < 0 || wx >= cw || wy >= cw) {
        continue;
      }
      blend_pixel_over(&fb[wy * cw + wx], col, a);
    }
  }
}

static void draw_tach_needle(uint16_t *fb, int cw, float ls, uint32_t rpm) {
  float v = (float)rpm;
  if (v > 8000.0f) {
    v = 8000.0f;
  }
  float ratio = v / 6000.0f;
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }
  float ang = map_angle(ratio, 115.0f, 250.0f);
  float mdeg = user_to_math_deg(ang) * (float)M_PI / 180.0f;
  int cx = (int)(0.5f * LAYOUT_REF * ls);
  int cy = (int)(0.42f * LAYOUT_REF * ls);
  int len = (int)(0.35f * LAYOUT_REF * ls);
  int tx = cx + (int)(cosf(mdeg) * (float)len);
  int ty = cy - (int)(sinf(mdeg) * (float)len);
  draw_line_thick(fb, cw, cw, cx, cy, tx, ty, (uint16_t)((31 << 11) | (6 << 5) | 5), 4);
}

static void clock_user_angles(uint32_t millis_val, float *minute_user, float *hour_user) {
  const uint32_t ms = millis_val;
  const uint32_t sec_total = ms / 1000u;
  const uint32_t min_of_hour = (sec_total / 60u) % 60u;
  const uint32_t sec_of_min = sec_total % 60u;
  const uint32_t hr12 = (sec_total / 3600u) % 12u;
  const float sec_frac = (float)(ms % 1000u) / 1000.0f;
  const float minute_ratio = (float(min_of_hour) + (float(sec_of_min) + sec_frac) / 60.0f) / 60.0f;
  const float hour_ratio = (float(hr12) + minute_ratio) / 12.0f;
  // Hands matched dial after removing 180° sprite offset (same as minute).
  *minute_user = fmodf(minute_ratio * 360.0f, 360.0f);
  *hour_user = fmodf(hour_ratio * 360.0f, 360.0f);
}

static void clock_user_angles_wall(uint64_t epoch_ms, float *minute_user, float *hour_user) {
  const time_t t = (time_t)(epoch_ms / 1000u);
  struct tm lt {};
  localtime_r(&t, &lt);
  const uint32_t ms = (uint32_t)(epoch_ms % 1000u);
  const float sec_frac = (float)ms / 1000.0f;
  const float sec_of_min = (float)lt.tm_sec + sec_frac;
  const float min_of_hour = (float)lt.tm_min + sec_of_min / 60.0f;
  const int hr12 = (lt.tm_hour % 12 + 12) % 12;
  const float minute_ratio = min_of_hour / 60.0f;
  const float hour_ratio = (float(hr12) + minute_ratio) / 12.0f;
  *minute_user = fmodf(minute_ratio * 360.0f, 360.0f);
  *hour_user = fmodf(hour_ratio * 360.0f, 360.0f);
}

#if CENTER_CLOCK_NEEDLE_HOUR_W > 0
static void draw_clock_hands_sprite(uint16_t *fb, int cw, uint32_t millis_boot, bool wall, uint64_t epoch_ms) {
  float mu = 0, hu = 0;
  if (wall) {
    clock_user_angles_wall(epoch_ms, &mu, &hu);
  } else {
    clock_user_angles(millis_boot, &mu, &hu);
  }
  blend_rotated_needle(fb, cw, center_clock_needle_hour_rgb565, center_clock_needle_hour_alpha, CENTER_CLOCK_NEEDLE_HOUR_W,
                       CENTER_CLOCK_NEEDLE_HOUR_H, hu);
  blend_rotated_needle(fb, cw, center_clock_needle_minute_rgb565, center_clock_needle_minute_alpha,
                       CENTER_CLOCK_NEEDLE_MINUTE_W, CENTER_CLOCK_NEEDLE_MINUTE_H, mu);
}
#endif

static void draw_clock_hands_line(uint16_t *fb, int cw, float ls, uint32_t millis_boot, bool wall, uint64_t epoch_ms) {
  const int dim = cw;
  const int cx = (int)(0.5f * LAYOUT_REF * ls);
  const int cy = (int)(0.5f * LAYOUT_REF * ls);
  float mu = 0, hu = 0;
  if (wall) {
    clock_user_angles_wall(epoch_ms, &mu, &hu);
  } else {
    clock_user_angles(millis_boot, &mu, &hu);
  }
  const float min_math = user_to_math_deg(mu) * (float)M_PI / 180.0f;
  const float hr_math = user_to_math_deg(hu) * (float)M_PI / 180.0f;
  const int mlen = (int)(0.30f * (float)dim);
  const int hlen = (int)(0.20f * (float)dim);
  const uint16_t wcol = (uint16_t)(((224 >> 3) << 11) | ((76 >> 2) << 5) | (56 >> 3));
  draw_line_thick(fb, cw, cw, cx, cy, cx + (int)(cosf(min_math) * mlen), cy - (int)(sinf(min_math) * mlen), wcol, 3);
  draw_line_thick(fb, cw, cw, cx, cy, cx + (int)(cosf(hr_math) * hlen), cy - (int)(sinf(hr_math) * hlen), wcol, 4);
}

static void scale_blit(Arduino_GFX *gfx, int16_t sw, int16_t sh, uint16_t *fb, int cw) {
  int out_s = (sw < sh) ? (int)sw : (int)sh;
  uint16_t *blit_buf = fb;
  int bw = cw;
  if (out_s != cw && out_s > 0) {
    static uint16_t *sc = nullptr;
    static size_t cap = 0;
    size_t need = (size_t)out_s * (size_t)out_s;
    if (need > cap || !sc) {
      free(sc);
#if defined(ESP_PLATFORM)
      sc = (uint16_t *)heap_caps_malloc(need * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
      sc = (uint16_t *)malloc(need * sizeof(uint16_t));
#endif
      cap = sc ? need : 0;
    }
    if (sc) {
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
          sc[yo * out_s + xo] = fb[yi * cw + xi];
        }
      }
      blit_buf = sc;
      bw = out_s;
    }
  }
  int16_t dx = (int16_t)((sw - bw) / 2);
  int16_t dy = (int16_t)((sh - bw) / 2);
  gfx->draw16bitRGBBitmap(dx, dy, blit_buf, bw, bw);
}

static void draw_classic_media(Arduino_GFX *gfx, int16_t sw, int16_t sh, const CenterGaugeInputs &in) {
  gfx->fillScreen(0x0000);
  gfx->setFont(nullptr);
  gfx->setTextSize(1);
  gfx->setTextColor(0xFFFFu);
  gfx->setCursor((int16_t)(sw / 2 - 52), 24);
  gfx->print("BLUETOOTH");
  char tmp[20];
  snprintf(tmp, sizeof(tmp), "%.0f F", (double)in.coolant_temp_f);
  gfx->setCursor((int16_t)(sw - 72), 44);
  gfx->print(tmp);
  gfx->setTextColor(0x07E0u);
  gfx->setCursor(24, (int16_t)(sh / 2 - 10));
  const char *np = center_audio_link_line();
  if (np && np[0]) {
    gfx->print(np);
  } else {
    gfx->setTextColor(0x7BCFu);
    gfx->print("No track metadata");
  }
  gfx->setTextColor(0x8410u);
  gfx->setCursor(24, (int16_t)(sh - 48));
  gfx->print("Media view");
}

void center_render(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, CenterViewMode mode, ClusterTheme theme,
                   const CenterGaugeInputs *inputs) {
  if (!gfx || !inputs) {
    return;
  }
  if (theme == ClusterTheme::Fallout) {
    center_draw_fallout(gfx, screen_w, screen_h, *inputs);
    return;
  }
  if (mode == CenterViewMode::Media) {
    draw_classic_media(gfx, screen_w, screen_h, *inputs);
    return;
  }

  const uint32_t engine_rpm = inputs->engine_rpm;
  const uint32_t millis_boot = inputs->millis_boot;
  const bool clock_use_wall_time = inputs->clock_wall;
  const uint64_t epoch_ms = inputs->epoch_ms;

  const int cw = CENTER_ASSET_EDGE;
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

  if (mode == CenterViewMode::Tach) {
    draw_tach_needle(fb, cw, ls, engine_rpm);

    size_t px = (size_t)cw * (size_t)cw;
    static uint16_t *d565 = nullptr;
    static uint8_t *dA = nullptr;
    if (!d565) {
#if defined(ESP_PLATFORM)
      d565 = (uint16_t *)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      dA = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
      d565 = (uint16_t *)malloc(px * sizeof(uint16_t));
      dA = (uint8_t *)malloc(px);
#endif
      if (d565 && dA) {
#if defined(ESP_PLATFORM)
        memcpy_P(d565, center_tach_dial_rgb565, px * sizeof(uint16_t));
        memcpy_P(dA, center_tach_dial_alpha, px);
#else
        memcpy(d565, center_tach_dial_rgb565, px * sizeof(uint16_t));
        memcpy(dA, center_tach_dial_alpha, px);
#endif
      }
    }
    if (d565 && dA) {
      blend_src_over(fb, d565, dA, (int)px);
    }
  } else {
#if CENTER_CLOCK_DIAL_HAVE
    {
      const int dw = CENTER_CLOCK_DIAL_W;
      const int dh = CENTER_CLOCK_DIAL_H;
      const size_t dial_px = (size_t)dw * (size_t)dh;
      static uint16_t *cd565 = nullptr;
      static uint8_t *cdA = nullptr;
      if (!cd565) {
#if defined(ESP_PLATFORM)
        cd565 = (uint16_t *)heap_caps_malloc(dial_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        cdA = (uint8_t *)heap_caps_malloc(dial_px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        cd565 = (uint16_t *)malloc(dial_px * sizeof(uint16_t));
        cdA = (uint8_t *)malloc(dial_px);
#endif
        if (cd565 && cdA) {
#if defined(ESP_PLATFORM)
          memcpy_P(cd565, center_clock_dial_rgb565, dial_px * sizeof(uint16_t));
          memcpy_P(cdA, center_clock_dial_alpha, dial_px);
#else
          memcpy(cd565, center_clock_dial_rgb565, dial_px * sizeof(uint16_t));
          memcpy(cdA, center_clock_dial_alpha, dial_px);
#endif
        }
      }
      if (cd565 && cdA) {
        const int ox = (cw - dw) / 2;
        const int oy = (cw - dh) / 2;
        blend_src_over_rect(fb, cw, cd565, cdA, dw, dh, ox, oy);
      }
    }
#endif

#if CENTER_CLOCK_NEEDLE_HOUR_W > 0
    draw_clock_hands_sprite(fb, cw, millis_boot, clock_use_wall_time, epoch_ms);
#else
    draw_clock_hands_line(fb, cw, ls, millis_boot, clock_use_wall_time, epoch_ms);
#endif

#if CENTER_CLOCK_OVERLAY_HAVE
    {
      size_t px = (size_t)cw * (size_t)cw;
      static uint16_t *ov565 = nullptr;
      static uint8_t *ovA = nullptr;
      if (!ov565) {
#if defined(ESP_PLATFORM)
        ov565 = (uint16_t *)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ovA = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        ov565 = (uint16_t *)malloc(px * sizeof(uint16_t));
        ovA = (uint8_t *)malloc(px);
#endif
        if (ov565 && ovA) {
#if defined(ESP_PLATFORM)
          memcpy_P(ov565, center_clock_overlay_rgb565, px * sizeof(uint16_t));
          memcpy_P(ovA, center_clock_overlay_alpha, px);
#else
          memcpy(ov565, center_clock_overlay_rgb565, px * sizeof(uint16_t));
          memcpy(ovA, center_clock_overlay_alpha, px);
#endif
        }
      }
      if (ov565 && ovA) {
        blend_src_over(fb, ov565, ovA, (int)px);
      }
    }
#endif
  }

  scale_blit(gfx, screen_w, screen_h, fb, cw);
}
