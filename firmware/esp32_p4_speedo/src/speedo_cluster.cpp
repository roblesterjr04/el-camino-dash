#include "speedo_cluster.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include <pgmspace.h>
#endif

#include "speedo_assets_gen.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const uint16_t BG_565 = 0x0000;  // #000
// Dial art (speedo_empty) is scaled to this fraction of the offscreen buffer; mask stays full cw×cw.
static const float SPEEDO_DIAL_SCALE = 0.75f;

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

#if defined(ESP_PLATFORM)
static uint16_t rd_needle565(const uint16_t *p, int i) { return (uint16_t)pgm_read_word(p + i); }
static uint8_t rd_needleA(const uint8_t *p, int i) { return pgm_read_byte(p + i); }
#else
static uint16_t rd_needle565(const uint16_t *p, int i) { return p[i]; }
static uint8_t rd_needleA(const uint8_t *p, int i) { return p[i]; }
#endif

static void blend_pixel_over(uint16_t *dst, uint16_t src565, uint8_t a) {
  if (a < 4) {
    return;
  }
  if (a > 250) {
    *dst = src565;
    return;
  }
  uint16_t d = *dst;
  uint32_t sr = (src565 >> 11) & 0x1F;
  uint32_t sg = (src565 >> 5) & 0x3F;
  uint32_t sb = src565 & 0x1F;
  uint32_t dr = (d >> 11) & 0x1F;
  uint32_t dg = (d >> 5) & 0x3F;
  uint32_t db = d & 0x1F;
  uint32_t ia = 255 - a;
  *dst = (uint16_t)((((sr * a + dr * ia) / 255) << 11) | (((sg * a + dg * ia) / 255) << 5) | ((sb * a + db * ia) / 255));
}

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

/** Bilinear sample in needle space (fills gaps vs forward splatting at diagonals). */
static void sample_needle_bilinear(const uint16_t *src565, const uint8_t *al, int sw, int sh, float sx, float sy,
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
  uint16_t c00 = rd_needle565(src565, y0 * sw + x0);
  uint16_t c10 = rd_needle565(src565, y0 * sw + x1);
  uint16_t c01 = rd_needle565(src565, y1 * sw + x0);
  uint16_t c11 = rd_needle565(src565, y1 * sw + x1);
  unpack565_u8(c00, &r00, &g00, &b00);
  unpack565_u8(c10, &r10, &g10, &b10);
  unpack565_u8(c01, &r01, &g01, &b01);
  unpack565_u8(c11, &r11, &g11, &b11);
  const uint32_t a00 = (uint32_t)rd_needleA(al, y0 * sw + x0);
  const uint32_t a10 = (uint32_t)rd_needleA(al, y0 * sw + x1);
  const uint32_t a01 = (uint32_t)rd_needleA(al, y1 * sw + x0);
  const uint32_t a11 = (uint32_t)rd_needleA(al, y1 * sw + x1);

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

/** Rotated needle: inverse map + bilinear (no single-pixel holes at arbitrary angles). */
static void blend_rotated_speedo_needle(uint16_t *fb, int cw, const uint16_t *src565, const uint8_t *alpha, int sw, int sh,
                                       float user_angle_deg, float pivot_x_frac, float pivot_y_frac, float hub_x,
                                       float hub_y, float sprite_angle_offset_deg) {
  if (sw < 2 || sh < 2) {
    return;
  }
  const float px = pivot_x_frac * (float)sw;
  const float py = pivot_y_frac * (float)sh;
  float deg = fmodf(user_to_math_deg(user_angle_deg) + sprite_angle_offset_deg + 1080.0f, 360.0f);
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
    const float wy = hub_y + dx * si + dy * co;
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
      const float dx = vxf * co + vyf * si;
      const float dyf = -vxf * si + vyf * co;
      const float sx = dx + px;
      const float sy = dyf + py;
      uint16_t col = 0;
      uint8_t aa = 0;
      sample_needle_bilinear(src565, alpha, sw, sh, sx, sy, &col, &aa);
      if (aa < 4) {
        continue;
      }
      blend_pixel_over(&fb[wy * cw + wx], col, aa);
    }
  }
}

static void draw_speed_needle_sprite(uint16_t *fb, int cw, float mph) {
  const float v0 = 0.0f;
  const float v1 = 140.0f;
  /* User degrees on the dial art: sweep from a0 (one end of arc) to a1 (other). */
  const float a0 = 60.0f;
  const float a1 = 300.0f;
  float ratio = (v1 <= v0) ? 0.0f : (mph - v0) / (v1 - v0);
  ratio = fmaxf(0.0f, fminf(1.0f, ratio));
  /* Mirror along the arc: 0 mph at a1 side, max mph at a0 (classic 0 on the left). */
  float user_ang = map_angle(1.0f - ratio, a0, a1);
  const float hub_x = 0.5f * (float)cw;
  const float hub_y = 0.5f * (float)cw;
  /* Theme sprite_render.speedometer.needle: pivot [0.5, 0.91], angle_offset_deg -90 */
  blend_rotated_speedo_needle(fb, cw, speedo_needle_rgb565, speedo_needle_alpha, SPEEDO_NEEDLE_W, SPEEDO_NEEDLE_H,
                              user_ang, 0.5f, 0.91f, hub_x, hub_y, -90.0f);
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

/** Nearest-neighbor scale cw×cw RGBA dial assets into a centered dial_s×dial_s region (matches TSPS classic1 helper). */
static void blend_scaled_src_over_rect(uint16_t *fb, int cw, const uint16_t *src565, const uint8_t *alpha, int dial_s, int mx,
                                       int my) {
  for (int dy = 0; dy < dial_s; dy++) {
    for (int dx = 0; dx < dial_s; dx++) {
      int sx = (dx * cw) / dial_s;
      int sy = (dy * cw) / dial_s;
      if (sx >= cw) {
        sx = cw - 1;
      }
      if (sy >= cw) {
        sy = cw - 1;
      }
      int si = sy * cw + sx;
      uint8_t a = alpha[si];
      if (a < 4) {
        continue;
      }
      uint16_t s = src565[si];
      int di = (my + dy) * cw + (mx + dx);
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

static uint16_t rgb565_u8(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | ((uint16_t)b >> 3));
}

// Chroma for odometer scratch canvas (unlikely on dial art); not drawn to gauge FB.
static constexpr uint16_t ODO_TEXT_KEY565 = 0xF81F;
static constexpr int16_t ODO_CANVAS_W = 256;
static constexpr int16_t ODO_CANVAS_H = 56;

/** Odometer into offscreen gauge buffer so scale_blit does not erase it (avoids flicker vs post-blit text).
 *  When `uart_boot_line` is set (sensor_pack boot banner), it uses the same slot and font scale as the ODO. */
static void draw_speedo_odometer_into_fb(uint16_t *fb, int cw, Arduino_GFX *gfx, float miles_m,
                                         bool sensor_pack_odometer_valid, const char *uart_boot_line) {
  if (!fb || !gfx) {
    return;
  }
  char buf[24];
  if (uart_boot_line && uart_boot_line[0]) {
    snprintf(buf, sizeof(buf), "%s", uart_boot_line);
  } else if (!sensor_pack_odometer_valid) {
    // Placeholder only when UART has not delivered a v3 odometer (stale link or legacy packet).
    snprintf(buf, sizeof(buf), "000000");
  } else {
    if (miles_m < 0.0f) {
      miles_m = 0.0f;
    }
    if (miles_m > 999999.9f) {
      miles_m = 999999.9f;
    }
    snprintf(buf, sizeof(buf), "%.1f", (double)miles_m);
  }

  static Arduino_Canvas *s_odo_canvas = nullptr;
  if (!s_odo_canvas) {
    s_odo_canvas = new Arduino_Canvas(ODO_CANVAS_W, ODO_CANVAS_H, gfx, 0, 0, 0);
    if (!s_odo_canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      return;
    }
  }

  s_odo_canvas->fillScreen(ODO_TEXT_KEY565);
  s_odo_canvas->setFont(nullptr);
  // One step larger than before (was 1 / 2 from asset width).
  const uint8_t ts = (cw >= 420) ? 3 : 2;
  s_odo_canvas->setTextSize(ts);
  // Single-arg setTextColor → bg not painted (transparent) on GLCD font.
  s_odo_canvas->setTextColor(rgb565_u8(236, 236, 226));

  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t tw = 0;
  uint16_t th = 0;
  s_odo_canvas->getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
  const int16_t cx = (int16_t)((ODO_CANVAS_W - (int)tw) / 2 - x1);
  const int16_t cy = (int16_t)((ODO_CANVAS_H - (int)th) / 2 - y1);
  s_odo_canvas->setCursor(cx, cy);
  s_odo_canvas->print(buf);

  const int hub_x = cw / 2;
  const int hub_y = cw / 2;
  const int y_down = (int)((50.0f * (float)cw) / (float)SPEEDO_ASSET_EDGE + 0.5f);
  const int anchor_y = hub_y + y_down;
  const int dest_x = hub_x - (int)ODO_CANVAS_W / 2;
  const int dest_y = anchor_y - (int)ODO_CANVAS_H / 2;

  uint16_t *cb = s_odo_canvas->getFramebuffer();
  for (int16_t yy = 0; yy < ODO_CANVAS_H; yy++) {
    const int dy = dest_y + (int)yy;
    if (dy < 0 || dy >= cw) {
      continue;
    }
    for (int16_t xx = 0; xx < ODO_CANVAS_W; xx++) {
      const int dx = dest_x + (int)xx;
      if (dx < 0 || dx >= cw) {
        continue;
      }
      const uint16_t p = cb[(int32_t)yy * ODO_CANVAS_W + (int32_t)xx];
      if (p != ODO_TEXT_KEY565) {
        fb[dy * cw + dx] = p;
      }
    }
  }
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
}

void speedo_render(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, float mph, float odometer_miles,
                   bool sensor_pack_odometer_valid, const char *uart_banner) {
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
  for (int i = 0; i < cw * cw; i++) {
    fb[i] = BG_565;
  }

  int dial_s = (int)roundf((float)cw * SPEEDO_DIAL_SCALE);
  if (dial_s < 2) {
    dial_s = (cw >= 2) ? 2 : 1;
  }
  int mx = (cw - dial_s) / 2;
  int my = (cw - dial_s) / 2;

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
    blend_scaled_src_over_rect(fb, cw, d565, dA, dial_s, mx, my);
  }

  draw_speed_needle_sprite(fb, cw, mph);

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

  draw_speedo_odometer_into_fb(fb, cw, gfx, odometer_miles, sensor_pack_odometer_valid, uart_banner);
  scale_blit(gfx, screen_w, screen_h, fb, cw);
  gfx->flush(true);
}
