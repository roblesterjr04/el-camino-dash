#include "center_cluster.h"

#include <sensor_link.h>

#include "center_media_ui.h"
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
#include "center_mpg_assets_gen.h"
#include <FreeSansBold10pt7b.h>
#include <el_camino_firmware_config.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Clock needle pivot in sprite space (fraction of exported bitmap w/h). Silver hub is at the bottom of
 * needle.png; must match gauge_screen._draw_clock_sprite_hand local_pivot (default [0.5, 0.94]).
 * Defined here (not only in el_camino_firmware_config.h) so a stale copy under .pio/libdeps cannot
 * shadow the repo and revert to center (0.5, 0.5) rotation. */
static constexpr float kClockNeedlePivotX = 0.5f;
static constexpr float kClockNeedlePivotY = 0.94f;
/* Matches gauge_screen._draw_clock_sprite_hand angle_offset_deg default (-90). */
static constexpr float kClockSpriteAngleOffsetDeg = -90.0f;
/* Dial vs sprite alignment: rotate hands 180° CW from prior firmware tuning. */
static constexpr float kClockHandRotateFixDeg = 180.0f;

static float user_to_math_deg(float u) { return fmodf(270.0f - u, 360.0f); }

/* Tach hub and tick geometry use the center framebuffer edge `cw` (view size); fractions from el_camino_firmware_config.h. */
static inline void tach_hub_px(int cw, int *cx, int *cy) {
  *cx = (int)((float)cw * EL_CAMINO_CENTER_TACH_HUB_X_FRAC_OF_VIEW + 0.5f);
  *cy = (int)((float)cw * EL_CAMINO_CENTER_TACH_HUB_Y_FRAC_OF_VIEW + 0.5f);
}

static inline int tach_radius_from_view_frac(int cw, float frac_of_view) {
  const int r = (int)((float)cw * frac_of_view + 0.5f) - EL_CAMINO_CENTER_TACH_TICK_RADIUS_TRIM_PX;
  return r > 2 ? r : 2;
}

static void plot4(uint16_t *fb, int w, int h, int x, int y, uint16_t c) {
  if (x >= 0 && y >= 0 && x < w && y < h) {
    fb[y * w + x] = c;
  }
}

static void draw_line_thick(uint16_t *fb, int w, int h, int x0, int y0, int x1, int y1, uint16_t c, int thick) {
  const int steps = EL_CAMINO_LINE_DRAW_STEPS_CENTER;
  for (int i = 0; i <= steps; i++) {
    float t = (float)i / (float)steps;
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

void center_blend_src_over(uint16_t *dst, const uint16_t *src565, const uint8_t *alpha, int n) {
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

static void unpack565_u8(uint16_t c, uint32_t *r, uint32_t *g, uint32_t *b) {
  *r = ((uint32_t)(c >> 11) & 31U) * 255U / 31U;
  *g = ((uint32_t)(c >> 5) & 63U) * 255U / 63U;
  *b = ((uint32_t)c & 31U) * 255U / 31U;
}

static uint16_t pack565_u8(uint32_t r, uint32_t g, uint32_t b) {
  if (r > 255U) {
    r = 255U;
  }
  if (g > 255U) {
    g = 255U;
  }
  if (b > 255U) {
    b = 255U;
  }
  uint32_t rr = (r * 31U) / 255U;
  uint32_t gg = (g * 63U) / 255U;
  uint32_t bb = (b * 31U) / 255U;
  if (rr > 31U) {
    rr = 31U;
  }
  if (gg > 63U) {
    gg = 63U;
  }
  if (bb > 31U) {
    bb = 31U;
  }
  return (uint16_t)((rr << 11) | (gg << 5) | bb);
}

static void sample_sprite_bilinear(const uint16_t *src565, const uint8_t *al, int sw, int sh, float sx, float sy,
                                   uint16_t *out565, uint8_t *out_a) {
  if (sx < -0.5f || sy < -0.5f || sx > (float)sw - 0.5f || sy > (float)sh - 0.5f) {
    *out_a = 0;
    return;
  }
  if (sx < 0.f) {
    sx = 0.f;
  }
  if (sy < 0.f) {
    sy = 0.f;
  }
  if (sx > (float)(sw - 1)) {
    sx = (float)(sw - 1);
  }
  if (sy > (float)(sh - 1)) {
    sy = (float)(sh - 1);
  }
  int x0 = (int)floorf(sx);
  int y0 = (int)floorf(sy);
  if (x0 < 0) {
    x0 = 0;
  }
  if (y0 < 0) {
    y0 = 0;
  }
  int x1 = x0 + 1;
  int y1 = y0 + 1;
  if (x1 >= sw) {
    x1 = sw - 1;
  }
  if (y1 >= sh) {
    y1 = sh - 1;
  }
  const float fx = sx - (float)x0;
  const float fy = sy - (float)y0;
  const float w00 = (1.f - fx) * (1.f - fy);
  const float w10 = fx * (1.f - fy);
  const float w01 = (1.f - fx) * fy;
  const float w11 = fx * fy;

  uint32_t r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
  uint16_t c00 = rd565(src565, y0 * sw + x0);
  uint16_t c10 = rd565(src565, y0 * sw + x1);
  uint16_t c01 = rd565(src565, y1 * sw + x0);
  uint16_t c11 = rd565(src565, y1 * sw + x1);
  unpack565_u8(c00, &r00, &g00, &b00);
  unpack565_u8(c10, &r10, &g10, &b10);
  unpack565_u8(c01, &r01, &g01, &b01);
  unpack565_u8(c11, &r11, &g11, &b11);
  const uint32_t a00 = (uint32_t)rdA(al, y0 * sw + x0);
  const uint32_t a10 = (uint32_t)rdA(al, y0 * sw + x1);
  const uint32_t a01 = (uint32_t)rdA(al, y1 * sw + x0);
  const uint32_t a11 = (uint32_t)rdA(al, y1 * sw + x1);

  const float rf = w00 * (float)r00 + w10 * (float)r10 + w01 * (float)r01 + w11 * (float)r11;
  const float gf = w00 * (float)g00 + w10 * (float)g10 + w01 * (float)g01 + w11 * (float)g11;
  const float bf = w00 * (float)b00 + w10 * (float)b10 + w01 * (float)b01 + w11 * (float)b11;
  const float af = w00 * (float)a00 + w10 * (float)a10 + w01 * (float)a01 + w11 * (float)a11;

  if (af < 4.f) {
    *out_a = 0;
    return;
  }
  *out565 = pack565_u8((uint32_t)(rf + 0.5f), (uint32_t)(gf + 0.5f), (uint32_t)(bf + 0.5f));
  uint32_t ai = (uint32_t)(af + 0.5f);
  if (ai > 255U) {
    ai = 255U;
  }
  *out_a = (uint8_t)ai;
}

/* Rotated sprite: inverse map + bilinear (matches gauge_screen; no diagonal holes). */
static void blend_rotated_needle(uint16_t *fb, int cw, const uint16_t *src565, const uint8_t *alpha, int sw, int sh,
                                 float user_angle_deg, float pivot_x_frac, float pivot_y_frac, float hub_x,
                                 float hub_y) {
  if (sw < 2 || sh < 2) {
    return;
  }
  const float px = pivot_x_frac * (float)sw;
  const float py = pivot_y_frac * (float)sh;
  const float deg = user_to_math_deg(user_angle_deg) + kClockSpriteAngleOffsetDeg + kClockHandRotateFixDeg;
  const float th = deg * (float)M_PI / 180.0f;
  const float co = cosf(th);
  const float si = sinf(th);

  float min_wx = 1e9f;
  float max_wx = -1e9f;
  float min_wy = 1e9f;
  float max_wy = -1e9f;
  for (int k = 0; k < 4; k++) {
    const float ix = (k & 1) ? (float)(sw - 1) : 0.f;
    const float iy = (k & 2) ? (float)(sh - 1) : 0.f;
    const float dx = ix - px;
    const float dy = iy - py;
    const float wx = hub_x + dx * co - dy * si;
    const float wy = hub_y - (dx * si + dy * co);
    if (wx < min_wx) {
      min_wx = wx;
    }
    if (wx > max_wx) {
      max_wx = wx;
    }
    if (wy < min_wy) {
      min_wy = wy;
    }
    if (wy > max_wy) {
      max_wy = wy;
    }
  }
  int x0 = (int)floorf(min_wx) - 1;
  int x1 = (int)ceilf(max_wx) + 1;
  int y0 = (int)floorf(min_wy) - 1;
  int y1 = (int)ceilf(max_wy) + 1;
  if (x0 < 0) {
    x0 = 0;
  }
  if (y0 < 0) {
    y0 = 0;
  }
  if (x1 >= cw) {
    x1 = cw - 1;
  }
  if (y1 >= cw) {
    y1 = cw - 1;
  }
  if (x0 > x1 || y0 > y1) {
    return;
  }

  for (int wy = y0; wy <= y1; wy++) {
    for (int wx = x0; wx <= x1; wx++) {
      const float vxf = (float)wx + 0.5f - hub_x;
      const float vyf = (float)wy + 0.5f - hub_y;
      const float dx = co * vxf - si * vyf;
      const float dyf = -si * vxf - co * vyf;
      const float sx = dx + px;
      const float sy = dyf + py;
      uint16_t col = 0;
      uint8_t aa = 0;
      sample_sprite_bilinear(src565, alpha, sw, sh, sx, sy, &col, &aa);
      if (aa < 4) {
        continue;
      }
      blend_pixel_over(&fb[wy * cw + wx], col, aa);
    }
  }
}

void center_blend_rotated_needle_like_clock(uint16_t *fb, int cw, const uint16_t *src565, const uint8_t *alpha, int sw,
                                            int sh, float user_angle_deg, float pivot_x_frac, float pivot_y_frac) {
  const float h = 0.5f * (float)cw;
  blend_rotated_needle(fb, cw, src565, alpha, sw, sh, user_angle_deg, pivot_x_frac, pivot_y_frac, h, h);
}

void center_blend_rotated_needle_like_clock_at(uint16_t *fb, int cw, const uint16_t *src565, const uint8_t *alpha,
                                               int sw, int sh, float user_angle_deg, float pivot_x_frac,
                                               float pivot_y_frac, float hub_x_px, float hub_y_px) {
  blend_rotated_needle(fb, cw, src565, alpha, sw, sh, user_angle_deg, pivot_x_frac, pivot_y_frac, hub_x_px,
                       hub_y_px);
}

static void tach_rpm_to_phi_deg(float rpm, float *out_phi) {
  float v = rpm;
  if (v > (float)EL_CAMINO_CENTER_TACH_RPM_CLAMP) {
    v = (float)EL_CAMINO_CENTER_TACH_RPM_CLAMP;
  }
  if (v < 0.f) {
    v = 0.f;
  }
  const float ratio = v / EL_CAMINO_CENTER_TACH_RPM_RATIO_REF;
  *out_phi = EL_CAMINO_CENTER_TACH_USER_DEG_0_RPM +
             ratio * (EL_CAMINO_CENTER_TACH_USER_DEG_MAX_RPM - EL_CAMINO_CENTER_TACH_USER_DEG_0_RPM);
}

/* User angle φ: 0° = horizontal left, increasing clockwise (0 RPM … 6000 RPM along the sweep). */
static void tach_phi_to_pixel_ray(float phi_deg, int cx, int cy, int r_inner, int r_outer, int *x0, int *y0, int *x1,
                                  int *y1) {
  const float math_deg = 180.0f - phi_deg;
  const float mdeg = math_deg * (float)M_PI / 180.0f;
  const float co = cosf(mdeg);
  const float si = sinf(mdeg);
  *x0 = cx + (int)(co * (float)r_inner);
  *y0 = cy - (int)(si * (float)r_inner);
  *x1 = cx + (int)(co * (float)r_outer);
  *y1 = cy - (int)(si * (float)r_outer);
}

static void draw_tach_ticks(uint16_t *fb, int cw) {
  int cx = 0, cy = 0;
  tach_hub_px(cw, &cx, &cy);
  const int r_outer = tach_radius_from_view_frac(cw, EL_CAMINO_CENTER_TACH_TICK_OUTER_FRAC_OF_VIEW);
  const int r_inner_major = tach_radius_from_view_frac(cw, EL_CAMINO_CENTER_TACH_TICK_INNER_MAJOR_FRAC_OF_VIEW);
  const int r_inner_minor = tach_radius_from_view_frac(cw, EL_CAMINO_CENTER_TACH_TICK_INNER_MINOR_FRAC_OF_VIEW);

  for (int major = 0; major <= 6; major++) {
    const float rpm = (float)major * 1000.0f;
    float phi = 0.f;
    tach_rpm_to_phi_deg(rpm, &phi);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    tach_phi_to_pixel_ray(phi, cx, cy, r_inner_major, r_outer, &x0, &y0, &x1, &y1);
    const uint16_t col =
        (major >= 4) ? EL_CAMINO_CENTER_TACH_TICK_LAST_MAJOR_RGB565 : EL_CAMINO_CENTER_TACH_TICK_MAJOR_RGB565;
    draw_line_thick(fb, cw, cw, x0, y0, x1, y1, col, EL_CAMINO_CENTER_TACH_TICK_MAJOR_THICK);
  }
  for (int minor = 1; minor < 12; minor++) {
    if (minor % 2 == 0) {
      continue;
    }
    const float rpm = (float)minor * 500.0f;
    float phi = 0.f;
    tach_rpm_to_phi_deg(rpm, &phi);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    tach_phi_to_pixel_ray(phi, cx, cy, r_inner_minor, r_outer, &x0, &y0, &x1, &y1);
    draw_line_thick(fb, cw, cw, x0, y0, x1, y1, EL_CAMINO_CENTER_TACH_TICK_MINOR_RGB565,
                    EL_CAMINO_CENTER_TACH_TICK_MINOR_THICK);
  }
}

static void draw_tach_needle(uint16_t *fb, int cw, uint32_t rpm) {
  float phi_deg = 0.f;
  tach_rpm_to_phi_deg((float)rpm, &phi_deg);
  const float math_deg = 180.0f - phi_deg;
  const float mdeg = math_deg * (float)M_PI / 180.0f;
  int cx = 0, cy = 0;
  tach_hub_px(cw, &cx, &cy);
  const int len = (int)((float)cw * EL_CAMINO_CENTER_TACH_NEEDLE_LEN_FRAC_OF_VIEW + 0.5f);
  int tx = cx + (int)(cosf(mdeg) * (float)len);
  int ty = cy - (int)(sinf(mdeg) * (float)len);
  draw_line_thick(fb, cw, cw, cx, cy, tx, ty, EL_CAMINO_CENTER_TACH_NEEDLE_RGB565, EL_CAMINO_CENTER_TACH_NEEDLE_LINE_W);
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
  /* Same convention as gauge_screen._draw_clock_hands: (180 + ratio*360) % 360. */
  *minute_user = fmodf(180.0f + minute_ratio * 360.0f, 360.0f);
  *hour_user = fmodf(180.0f + hour_ratio * 360.0f, 360.0f);
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
  *minute_user = fmodf(180.0f + minute_ratio * 360.0f, 360.0f);
  *hour_user = fmodf(180.0f + hour_ratio * 360.0f, 360.0f);
}

#if CENTER_CLOCK_NEEDLE_HOUR_W > 0
static void draw_clock_hands_sprite(uint16_t *fb, int cw, uint32_t millis_boot, bool wall, uint64_t epoch_ms) {
  float mu = 0, hu = 0;
  if (wall) {
    clock_user_angles_wall(epoch_ms, &mu, &hu);
  } else {
    clock_user_angles(millis_boot, &mu, &hu);
  }
  const float hub = 0.5f * (float)cw;
  blend_rotated_needle(fb, cw, center_clock_needle_hour_rgb565, center_clock_needle_hour_alpha, CENTER_CLOCK_NEEDLE_HOUR_W,
                       CENTER_CLOCK_NEEDLE_HOUR_H, hu, kClockNeedlePivotX, kClockNeedlePivotY, hub, hub);
  blend_rotated_needle(fb, cw, center_clock_needle_minute_rgb565, center_clock_needle_minute_alpha,
                       CENTER_CLOCK_NEEDLE_MINUTE_W, CENTER_CLOCK_NEEDLE_MINUTE_H, mu, kClockNeedlePivotX,
                       kClockNeedlePivotY, hub, hub);
}
#endif

static void draw_clock_hands_line(uint16_t *fb, int cw, float ls, uint32_t millis_boot, bool wall, uint64_t epoch_ms) {
  const int dim = cw;
  const float ref = (float)EL_CAMINO_GAUGE_LAYOUT_REF_PX;
  const int cx = (int)(EL_CAMINO_CENTER_CLOCK_LINE_HUB_X_FRAC * ref * ls);
  const int cy = (int)(EL_CAMINO_CENTER_CLOCK_LINE_HUB_Y_FRAC * ref * ls);
  float mu = 0, hu = 0;
  if (wall) {
    clock_user_angles_wall(epoch_ms, &mu, &hu);
  } else {
    clock_user_angles(millis_boot, &mu, &hu);
  }
  const float min_math = (user_to_math_deg(mu) + kClockHandRotateFixDeg) * (float)M_PI / 180.0f;
  const float hr_math = (user_to_math_deg(hu) + kClockHandRotateFixDeg) * (float)M_PI / 180.0f;
  const int mlen = (int)(EL_CAMINO_CENTER_CLOCK_LINE_MINUTE_LEN_FRAC * (float)dim);
  const int hlen = (int)(EL_CAMINO_CENTER_CLOCK_LINE_HOUR_LEN_FRAC * (float)dim);
  const uint16_t wcol = EL_CAMINO_CENTER_CLOCK_LINE_RGB565;
  draw_line_thick(fb, cw, cw, cx, cy, cx + (int)(cosf(min_math) * mlen), cy - (int)(sinf(min_math) * mlen), wcol,
                  EL_CAMINO_CENTER_CLOCK_LINE_MINUTE_THICK);
  draw_line_thick(fb, cw, cw, cx, cy, cx + (int)(cosf(hr_math) * hlen), cy - (int)(sinf(hr_math) * hlen), wcol,
                  EL_CAMINO_CENTER_CLOCK_LINE_HOUR_THICK);
}

static float mpg_needle_clamp_40(float v) {
  if (v < 0.f) {
    return 0.f;
  }
  if (v > 40.f) {
    return 40.f;
  }
  return v;
}

/* needle.png + clock pivot. Sprite user_deg endpoints tuned so increasing MPG follows dial convention
 * in el_camino_firmware_config.h (45° @ 0 MPG → 135° @ 40 MPG, 0° = far left, CW increasing). */
static constexpr float kMpgAvgUserDeg0 = 111.f;
static constexpr float kMpgAvgUserDeg1 = 201.f;
static constexpr float kMpgInstHubXFrac = 0.5f;
static constexpr float kMpgInstHubYFrac = 0.70f;
static constexpr float kMpgInstUserDeg0 = 102.f;
static constexpr float kMpgInstUserDeg1 = 198.f;

/** Average MPG: 0..40 → 45°..135° on dial (see firmware_config dial convention; classic_1/needle.png). */
static void draw_mpg_avg_needle(uint16_t *fb, int cw, float mpg) {
  const float m = mpg_needle_clamp_40(mpg);
  const float ratio = m / 40.f;
#if CENTER_MPG_AVG_NEEDLE_W > 0
  const float user_deg = fmodf(kMpgAvgUserDeg0 + ratio * (kMpgAvgUserDeg1 - kMpgAvgUserDeg0), 360.0f);
  center_blend_rotated_needle_like_clock(fb, cw, center_mpg_needle_avg_rgb565, center_mpg_needle_avg_alpha,
                                         CENTER_MPG_AVG_NEEDLE_W, CENTER_MPG_AVG_NEEDLE_H, user_deg, kClockNeedlePivotX,
                                         kClockNeedlePivotY);
#else
  const float phi = 45.f + ratio * 90.f;
  const float math_deg = 180.0f - phi;
  const float rad = math_deg * (float)M_PI / 180.0f;
  const int cx = cw / 2;
  const int cy = cw / 2;
  const int len = (int)((float)cw * 0.34f + 0.5f);
  const int tx = cx + (int)(cosf(rad) * (float)len);
  const int ty = cy - (int)(sinf(rad) * (float)len);
  draw_line_thick(fb, cw, cw, cx, cy, tx, ty, (uint16_t)0xD69Au, 3);
#endif
}

/** Instant MPG: smaller needle.png at sub-hub under “M”. */
static void draw_mpg_inst_needle(uint16_t *fb, int cw, float mpg) {
  const float m = mpg_needle_clamp_40(mpg);
  const float ratio = m / 40.f;
#if CENTER_MPG_INST_NEEDLE_W > 0
  const float user_deg = fmodf(kMpgInstUserDeg0 + ratio * (kMpgInstUserDeg1 - kMpgInstUserDeg0), 360.0f);
  const float hx = kMpgInstHubXFrac * (float)cw;
  const float hy = kMpgInstHubYFrac * (float)cw;
  center_blend_rotated_needle_like_clock_at(fb, cw, center_mpg_needle_inst_rgb565, center_mpg_needle_inst_alpha,
                                              CENTER_MPG_INST_NEEDLE_W, CENTER_MPG_INST_NEEDLE_H, user_deg,
                                              kClockNeedlePivotX, kClockNeedlePivotY, hx, hy);
#else
  const float phi = 158.f + ratio * 64.f;
  const float math_deg = 180.0f - phi;
  const float rad = math_deg * (float)M_PI / 180.0f;
  const int cx = (int)(kMpgInstHubXFrac * (float)cw + 0.5f);
  const int cy = (int)(kMpgInstHubYFrac * (float)cw + 0.5f);
  const int len = (int)((float)cw * 0.10f + 0.5f);
  if (len < 3) {
    return;
  }
  const int tx = cx + (int)(cosf(rad) * (float)len);
  const int ty = cy - (int)(sinf(rad) * (float)len);
  draw_line_thick(fb, cw, cw, cx, cy, tx, ty, (uint16_t)0xF800u, 2);
#endif
}

static void draw_mpg_dte_readout(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, bool valid, uint32_t dte_tenths) {
  const int out_s = (screen_w < screen_h) ? (int)screen_w : (int)screen_h;
  const int16_t dx = (int16_t)((screen_w - out_s) / 2);
  const int16_t dy = (int16_t)((screen_h - out_s) / 2);
  const float rel_y = 0.902f;
  /* Nudge band target upward on screen (smaller Y) vs rel_y anchor. */
  const int16_t cy = (int16_t)(dy + (int16_t)(rel_y * (float)out_s) - 8);
  char buf[32];
  if (valid) {
    const double mi = (double)dte_tenths * 0.1;
    if (mi >= 100.0) {
      snprintf(buf, sizeof(buf), "%.0f MI", mi);
    } else {
      snprintf(buf, sizeof(buf), "%.1f MI", mi);
    }
  } else {
    snprintf(buf, sizeof(buf), "--- MI");
  }
  gfx->setFont(&FreeSansBold10pt7b);
  gfx->setTextSize(2);
  gfx->setTextColor((uint16_t)0xFFFFu);
  int16_t bx = 0, by = 0;
  uint16_t tw = 0, th = 0;
  gfx->getTextBounds(buf, 0, 0, &bx, &by, &tw, &th);
  const int16_t tx = (int16_t)(dx + (out_s - (int16_t)tw) / 2);
  const int16_t baseline = (int16_t)(cy - by);
  gfx->setCursor(tx, baseline);
  gfx->print(buf);
  gfx->setTextSize(1);
  gfx->setFont(nullptr);
}

static void draw_clock_street_readout(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, const char *street) {
  if (!gfx || !street || !street[0]) {
    return;
  }
  const int out_s = (screen_w < screen_h) ? (int)screen_w : (int)screen_h;
  const int16_t dx = (int16_t)((screen_w - out_s) / 2);
  const int16_t dy = (int16_t)((screen_h - out_s) / 2);
  const float rel_y = 0.88f;
  const int16_t cy = (int16_t)(dy + (int16_t)(rel_y * (float)out_s));
  char buf[56];
  strncpy(buf, street, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  if (strlen(buf) > 44u) {
    buf[41] = '.';
    buf[42] = '.';
    buf[43] = '.';
    buf[44] = '\0';
  }
  gfx->setFont(&FreeSansBold10pt7b);
  gfx->setTextSize(1);
  gfx->setTextColor((uint16_t)0xCE79u);
  int16_t bx = 0, by = 0;
  uint16_t tw = 0, th = 0;
  gfx->getTextBounds(buf, 0, 0, &bx, &by, &tw, &th);
  const int16_t tx = (int16_t)(dx + (out_s - (int16_t)tw) / 2);
  const int16_t baseline = (int16_t)(cy - by);
  gfx->setCursor(tx, baseline);
  gfx->print(buf);
  gfx->setFont(nullptr);
}

void center_gauge_scale_blit(Arduino_GFX *gfx, int16_t sw, int16_t sh, uint16_t *fb, int cw) {
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


void center_render(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, CenterViewMode mode, ClusterTheme theme,
                   const CenterGaugeInputs *inputs) {
  if (!gfx || !inputs) {
    return;
  }
  static CenterViewMode s_prev_mode = CenterViewMode::Tach;
  const bool leaving_media =
      (s_prev_mode == CenterViewMode::Media) && (mode != CenterViewMode::Media || theme == ClusterTheme::Fallout);
  if (leaving_media) {
    center_media_lvgl_teardown();
  }
  s_prev_mode = mode;

  if (theme == ClusterTheme::Fallout) {
    center_draw_fallout(gfx, screen_w, screen_h, *inputs);
    return;
  }
  if (mode == CenterViewMode::Media) {
    center_draw_media_screen(gfx, screen_w, screen_h, *inputs);
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
  float ls = (float)cw / (float)EL_CAMINO_GAUGE_LAYOUT_REF_PX;
  for (int i = 0; i < cw * cw; i++) {
    fb[i] = EL_CAMINO_CENTER_BG_RGB565;
  }

  if (mode == CenterViewMode::Tach) {
    /* Ticks + needle sit under the dial artwork (bezel/glass on top). */
    draw_tach_ticks(fb, cw);
    draw_tach_needle(fb, cw, engine_rpm);
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
      center_blend_src_over(fb, d565, dA, (int)px);
    }
  } else if (mode == CenterViewMode::Efficiency) {
    /* Unlike tach (ticks under glass), MPG face art is mostly opaque — paint dial first, needles on top. */
#if CENTER_MPG_DIAL_HAVE
    {
      const size_t px = (size_t)cw * (size_t)cw;
      static uint16_t *m565 = nullptr;
      static uint8_t *mA = nullptr;
      if (!m565) {
#if defined(ESP_PLATFORM)
        m565 = (uint16_t *)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        mA = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        m565 = (uint16_t *)malloc(px * sizeof(uint16_t));
        mA = (uint8_t *)malloc(px);
#endif
        if (m565 && mA) {
#if defined(ESP_PLATFORM)
          memcpy_P(m565, center_mpg_dial_rgb565, px * sizeof(uint16_t));
          memcpy_P(mA, center_mpg_dial_alpha, px);
#else
          memcpy(m565, center_mpg_dial_rgb565, px * sizeof(uint16_t));
          memcpy(mA, center_mpg_dial_alpha, px);
#endif
        }
      }
      if (m565 && mA) {
        center_blend_src_over(fb, m565, mA, (int)px);
      }
    }
#endif
    draw_mpg_avg_needle(fb, cw, inputs->mpg_average);
    draw_mpg_inst_needle(fb, cw, inputs->mpg_instant);
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
        center_blend_src_over(fb, ov565, ovA, (int)px);
      }
    }
#endif
  }

  // The clock doesn't depend on the sensor pack; tach and efficiency do.
  if (!inputs->sensor_valid && mode != CenterViewMode::Clock) {
    elc_fb_stamp_no_data(gfx, fb, cw);
  }
  center_gauge_scale_blit(gfx, screen_w, screen_h, fb, cw);
  if (mode == CenterViewMode::Efficiency) {
    draw_mpg_dte_readout(gfx, screen_w, screen_h, inputs->fuel_economy_valid, inputs->dte_tenths_mile);
  } else if (mode == CenterViewMode::Clock) {
    draw_clock_street_readout(gfx, screen_w, screen_h, inputs->current_street);
  }
}
