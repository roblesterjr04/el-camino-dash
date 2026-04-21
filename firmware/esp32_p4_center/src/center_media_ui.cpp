#include "center_media_ui.h"

#include "center_audio_link.h"
#include "center_cluster.h"
#include "center_music_assets_gen.h"
#include "FreeMono8pt7b.h"

#include <Arduino.h>
#include <canvas/Arduino_Canvas.h>
#include <Arduino_GFX_Library.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** Gray arch ring in canvas pixels. Center is (cw/2, cw/2) → screen center after center_gauge_scale_blit. */
static constexpr int kMusicArchOuterRadiusPx = 160;
/** Inner radius: same 34/40 ratio as the previous cw-scaled ring so band thickness stays consistent. */
static constexpr int kMusicArchInnerRadiusPx = 110;

/** Same hub as clock sprite needle (needle.png — pivot at bottom of hub). */
static constexpr float kMusicNeedlePivotX = 0.5f;
static constexpr float kMusicNeedlePivotY = 0.94f;

static int16_t s_dim_sw = -1;
static int16_t s_dim_sh = -1;
static uint32_t s_last_layout_sig = 0xffffffffu;
static uint16_t s_last_elapsed_s = 0xffffu;
/** Sub-second needle: last integer elapsed we saw from I2C + millis() at that observation. */
static uint16_t s_tr_elapsed_anchor = 0xffffu;
static uint32_t s_tr_elapsed_anchor_ms = 0;

static Arduino_Canvas *s_canvas = nullptr;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint32_t hash_cstr(const char *p) {
  uint32_t h = 2166136261u;
  for (; p && *p; ++p) {
    h = (h ^ (uint32_t)(uint8_t)*p) * 16777619u;
  }
  return h;
}

static void sanitize_track(const char *in, char *out, size_t cap) {
  size_t j = 0;
  if (!in) {
    out[0] = '\0';
    return;
  }
  for (size_t i = 0; in[i] && j + 1 < cap; i++) {
    const unsigned char c = (unsigned char)in[i];
    if (c == '\t') {
      out[j++] = ' ';
    } else if (c >= 32 && c != 127) {
      /* AVRCP text is UTF-8; the old ASCII-only filter stripped every byte >= 127, wiping titles. */
      out[j++] = (char)c;
    }
  }
  out[j] = '\0';
}

/** Monospace characters on an arc in the upper void (same band as music_base CURRENT SONG art).
 * Arduino_GFX / cos: 0 deg = 3 o'clock, 180 deg = 9 o'clock (left), 270 deg = 12 o'clock (top).
 * Arc 210-330 deg follows the recessed gray arch in the asset.
 * drawChar: bg == fg skips the background fill (Arduino_GFX.cpp). */
static void draw_track_along_arc(Arduino_GFX *gfx, int cx, int cy, int r_text, const char *track, uint16_t fg) {
  gfx->setFont(&FreeMono8pt7b);
  gfx->setTextSize(1);

  const size_t n = strnlen(track, 44);
  if (n == 0) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    const float t = (n == 1) ? 0.5f : (float)i / (float)(n - 1);
    const float th_deg = 210.f + t * 120.f;
    const float th = th_deg * (float)M_PI / 180.f;
    const float xc = (float)cx + (float)r_text * cosf(th);
    const float yc = (float)cy + (float)r_text * sinf(th);
    char ch[2] = { track[i], '\0' };
    int16_t x1 = 0, y1 = 0;
    uint16_t w = 0, h = 0;
    gfx->getTextBounds(ch, 0, 0, &x1, &y1, &w, &h);
    const int16_t tx = (int16_t)(xc - (float)w * 0.5f - (float)x1);
    const int16_t ty = (int16_t)(yc - (float)h * 0.5f - (float)y1);
    gfx->drawChar(tx, ty, (unsigned char)track[i], fg, fg);
  }
}

void center_media_lvgl_teardown(void) {
  delete s_canvas;
  s_canvas = nullptr;
  s_dim_sw = -1;
  s_dim_sh = -1;
  s_last_layout_sig = 0xffffffffu;
  s_last_elapsed_s = 0xffffu;
  s_tr_elapsed_anchor = 0xffffu;
  s_tr_elapsed_anchor_ms = 0;
}

void center_media_lvgl_sync_overlay(bool hide_for_overlay) {
  (void)hide_for_overlay;
}

void center_draw_media_screen(Arduino_GFX *gfx, int16_t sw, int16_t sh, const CenterGaugeInputs &in) {
  if (!gfx || sw <= 0 || sh <= 0) {
    return;
  }

  const int cw = CENTER_MUSIC_BASE_W;
  if (cw < 8) {
    return;
  }

  const bool dims_changed = (sw != s_dim_sw || sh != s_dim_sh);

  const char *np_raw = center_audio_link_line();
  char track[48];
  sanitize_track(np_raw, track, sizeof(track));
  const bool have_title = track[0] != '\0';
  /* Do not draw "--" on the arc (two chars at 210° and 330° looked like stray dashes). */
  if (!have_title) {
    track[0] = '\0';
  }

  uint16_t elapsed_s = 0, duration_s = 0;
  uint8_t vol_u4 = 0;
  const bool tr_ok = center_audio_link_transport(&elapsed_s, &duration_s, &vol_u4);
  (void)vol_u4;

  const uint32_t line_h = hash_cstr(track);

  const uint32_t layout_sig =
      line_h ^ ((uint32_t)duration_s << 3) ^ ((uint32_t)(center_audio_link_ok() ? 1u : 0u) << 19) ^ (tr_ok ? 1u : 0u);

  const bool want_transport_anim = tr_ok && duration_s > 0u;
  /* Without this, we only repaint once per integer second and the needle appears stuck. */
  if (!want_transport_anim && !dims_changed && layout_sig == s_last_layout_sig && elapsed_s == s_last_elapsed_s) {
    return;
  }

  if (!s_canvas || s_canvas->width() != cw) {
    delete s_canvas;
    s_canvas = new Arduino_Canvas(cw, cw, gfx);
    if (!s_canvas || !s_canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      delete s_canvas;
      s_canvas = nullptr;
      return;
    }
  }

  uint16_t *fb = s_canvas->getFramebuffer();
  if (!fb) {
    return;
  }

  const int cx = cw / 2;
  const int cy_arch = cw / 2;

  const int r_outer = kMusicArchOuterRadiusPx;
  const int r_inner = kMusicArchInnerRadiusPx;
  const int r_text = (r_outer + r_inner) / 2;
  const uint16_t slot_gray = rgb565(72, 74, 80);
  const uint16_t track_fg = rgb565(255, 255, 255);

  s_canvas->startWrite();
  s_canvas->fillScreen(0x0000);
  /* Layer: gray archway under the sprite; music_base blends on top (void must be transparent in PNG). */
  s_canvas->fillArc((int16_t)cx, (int16_t)cy_arch, (int16_t)r_outer, (int16_t)r_inner, 230.f, 310.f,
                    slot_gray);
  center_blend_src_over(fb, center_music_base_rgb565, center_music_base_alpha, cw * cw);
  /* Title above artwork so it reads in the void. */
  draw_track_along_arc(s_canvas, cx, cy_arch, r_text, track, track_fg);
  s_canvas->endWrite();

  float progress = 0.f;
  if (want_transport_anim) {
    if (layout_sig != s_last_layout_sig || elapsed_s != s_tr_elapsed_anchor) {
      s_tr_elapsed_anchor = elapsed_s;
      s_tr_elapsed_anchor_ms = millis();
    }
    const float frac =
        fminf((float)(millis() - s_tr_elapsed_anchor_ms) / 1000.0f, 0.999f);
    float elapsed_f = (float)elapsed_s + frac;
    if (elapsed_f > (float)duration_s) {
      elapsed_f = (float)duration_s;
    }
    progress = elapsed_f / (float)duration_s;
    if (progress > 1.f) {
      progress = 1.f;
    }
    if (progress < 0.f) {
      progress = 0.f;
    }
  } else {
    s_tr_elapsed_anchor = 0xffffu;
  }

  /* Transport needle: full top semicircle, 9 o'clock (GFX 180 deg) → 3 o'clock (0 deg) through 12 o'clock.
   * That is phi = 180 + progress*180. Clock sprite: dial direction phi ~= user_angle + 90 deg → user 90..270. */
  const float user_angle_deg = fmodf(90.0f + progress * 180.0f, 360.0f);
  /* Pivot at dial center; sprite orientation matches clock needles on this asset. */
  center_blend_rotated_needle_like_clock(fb, cw, center_music_needle_rgb565, center_music_needle_alpha,
                                         CENTER_MUSIC_NEEDLE_W, CENTER_MUSIC_NEEDLE_H, user_angle_deg,
                                         kMusicNeedlePivotX, kMusicNeedlePivotY);

  center_gauge_scale_blit(gfx, sw, sh, fb, cw);

  s_last_layout_sig = layout_sig;
  s_last_elapsed_s = elapsed_s;
  s_dim_sw = sw;
  s_dim_sh = sh;
}
