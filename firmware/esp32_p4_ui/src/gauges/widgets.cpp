// The painter runs per pixel: always build it optimised, even in -Os firmware builds.
#pragma GCC optimize("O3", "fast-math")

#include "widgets.h"

#include <string.h>

namespace gauge {

namespace {
void shape_draw_cb(lv_event_t *e) {
  auto *sh = static_cast<Shape *>(lv_event_get_user_data(e));
  LvSurface s = LvSurface::from_event(e);
  sh->draw(s);
}
}  // namespace

void Shape::attach(lv_obj_t *parent) {
  obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_set_pos(obj, 0, 0);
  lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(obj, shape_draw_cb, LV_EVENT_DRAW_MAIN, this);
}

void Shape::invalidate(Box b) {
  if (!obj) {
    return;
  }
  lv_area_t a;
  a.x1 = (lv_coord_t)floorf(b.x0) - 1;
  a.y1 = (lv_coord_t)floorf(b.y0) - 1;
  a.x2 = (lv_coord_t)ceilf(b.x1) + 1;
  a.y2 = (lv_coord_t)ceilf(b.y1) + 1;
  lv_obj_invalidate_area(obj, &a);
}

// ---------------------------------------------------------------- Needle

namespace {
constexpr float kShadowDx = 3.0f, kShadowDy = 5.0f, kShadowBlur = 6.0f;
}

Box Needle::bbox(float at) const {
  const float ux = sinf(rad(at)), uy = -cosf(rad(at));
  const float r_in = style == NeedleStyle::Pointer ? r_start : -tail;
  const float x0 = cx + ux * r_in, y0 = cy + uy * r_in;
  const float x1 = cx + ux * len, y1 = cy + uy * len;
  const float m = fmaxf(w_base, w_tip) * 0.5f + 2.0f;
  Box b = box_seg(x0, y0, x1, y1, m);
  if (style != NeedleStyle::Line) {
    Box sb = b;
    sb.x0 += kShadowDx;
    sb.x1 += kShadowDx;
    sb.y0 += kShadowDy;
    sb.y1 += kShadowDy;
    b = box_union(b, box_grow(sb, kShadowBlur));
  }
  return b;
}

void Needle::set(float new_psi) {
  if (fabsf(new_psi - psi) < 0.04f) {
    return;
  }
  invalidate(bbox(psi));
  psi = new_psi;
  invalidate(bbox(psi));
}

void Needle::draw(LvSurface &s) {
  const float ux = sinf(rad(psi)), uy = -cosf(rad(psi));
  const float r_in = style == NeedleStyle::Pointer ? r_start : -tail;
  const float ax = cx + ux * r_in, ay = cy + uy * r_in;
  const float bx = cx + ux * len, by = cy + uy * len;
  const float ra = w_base * 0.5f, rb = w_tip * 0.5f;

  if (style == NeedleStyle::Line) {
    seg(s, ax, ay, bx, by, w_base, color);
    return;
  }
  // Soft shadow on the dial.
  const float sax = ax + kShadowDx, say = ay + kShadowDy, sbx = bx + kShadowDx, sby = by + kShadowDy;
  fill_along(s, sax, say, sbx, sby, fmaxf(ra, rb) + kShadowBlur, [&](float x, float y, Rgb &o) {
    const float d = sd_uneven_capsule(x, y, sax, say, ra, sbx, sby, rb);
    o = Rgb{0, 0, 0};
    return 0.45f * (1.0f - smooth(-2.0f, kShadowBlur, d));
  });
  // Blade: darker edges, a bright spine down the middle.
  const Rgb hi = style == NeedleStyle::Hand ? hex(0xffffff) : mix(color, hex(0xffd2c8), 0.55f);
  const Rgb lo = mul(color, 0.62f);
  fill_along(s, ax, ay, bx, by, fmaxf(ra, rb), [&](float x, float y, Rgb &o) {
    const float d = sd_uneven_capsule(x, y, ax, ay, ra, bx, by, rb);
    const float a = cover(d);
    if (a <= 0) {
      return 0.0f;
    }
    // across: 0 on the spine, 1 at the edge
    const float along = clampf(((x - ax) * ux + (y - ay) * uy) / fmaxf(len - r_in, 1.0f), 0.0f, 1.0f);
    const float hw = lerpf(ra, rb, along);
    const float across = clampf(fabsf(-(x - ax) * uy + (y - ay) * ux) / fmaxf(hw, 0.5f), 0.0f, 1.0f);
    const float sp = 1.0f - across, spine = sp * sp * sp;
    o = mix(mix(color, lo, across * across), hi, spine * 0.55f);
    return a;
  });
}

// ---------------------------------------------------------------- Hub

void Hub::draw(LvSurface &s) {
  // Shadow ring, then the ball.
  fill_annulus(s, box_circle(cx + 2, cy + 3, r + 6), cx + 2, cy + 3, r - 3, r + 5, [&](float x, float y, Rgb &o) {
    o = Rgb{0, 0, 0};
    return 0.5f * (1.0f - smooth(-2.0f, 5.0f, sd_circle(x, y, cx + 2, cy + 3, r)));
  });
  disc_shaded(s, cx, cy, r, [&](float x, float y) { return chrome_sphere(x, y, cx, cy, r); });
  if (screw_slot) {
    const float ux = cosf(rad(28)), uy = sinf(rad(28));
    bar(s, cx - ux * r * 0.8f, cy - uy * r * 0.8f, cx + ux * r * 0.8f, cy + uy * r * 0.8f, fmaxf(r * 0.22f, 1.5f),
        hex(0x202326), 0.85f);
  }
}

// ---------------------------------------------------------------- ArcFill

float ArcFill::end_psi(float f) const {
  return from_end ? psi1 - (psi1 - psi0) * f : psi0 + (psi1 - psi0) * f;
}

Box ArcFill::span_box(float fa, float fb) const {
  float a = end_psi(fa), b = end_psi(fb);
  if (a > b) {
    const float t = a;
    a = b;
    b = t;
  }
  return box_grow(box_arc_band(cx, cy, r - width * 0.5f, r + width * 0.5f, a - 0.5f, b + 0.5f), width * 0.6f + 2);
}

void ArcFill::set(float f) {
  f = clampf(f, 0.0f, 1.0f);
  if (fabsf((f - frac) * (psi1 - psi0)) < 0.05f) {
    return;
  }
  invalidate(span_box(frac, f));
  frac = f;
}

void ArcFill::set_override(bool on, Rgb c) {
  if (on == use_override && (!on || (c.r == override_c.r && c.g == override_c.g && c.b == override_c.b))) {
    return;
  }
  use_override = on;
  override_c = c;
  invalidate(span_box(0, frac));
}

void ArcFill::draw(LvSurface &s) {
  const float e = end_psi(frac);
  const float a = from_end ? e : psi0, b = from_end ? psi1 : e;
  if (b - a < 0.05f) {
    return;
  }
  const float r0 = r - width * 0.5f, r1 = r + width * 0.5f;
  const float span = psi1 - psi0;
  arc_band_shaded(s, cx, cy, r0, r1, a, b, round_caps, [&](float x, float y) {
    if (use_override) {
      return override_c;
    }
    float t = (wrap180(angle_of(x, y, cx, cy) - psi0 - 180.0f) + 180.0f) / span;  // 0 at psi0
    return mix(c0, c1, clampf(t, 0.0f, 1.0f));
  });
  if (tip) {
    const float tx = polar_x(cx, r, e), ty = polar_y(cy, r, e);
    disc(s, tx, ty, width * 0.32f, hex(0xffffff));
  }
}

// ---------------------------------------------------------------- Lamp

void Lamp::set(bool v) {
  if (v == lit) {
    return;
  }
  lit = v;
  invalidate(box_arc_band(cx, cy, r0, r1, psi0, psi1));
}

void Lamp::draw(LvSurface &s) {
  const float rm = (r0 + r1) * 0.5f, hw = (r1 - r0) * 0.5f;
  arc_band_shaded(s, cx, cy, r0, r1, psi0, psi1, false, [&](float x, float y) {
    const float t = clampf(fabsf(vlen(x - cx, y - cy) - rm) / hw, 0.0f, 1.0f);
    if (!lit) {
      return mix(off, mul(off, 0.6f), t);
    }
    return mix(mix(on, hex(0xffffff), 0.35f * (1.0f - t) * (1.0f - t)), mul(on, 0.7f), t * t);
  });
}

// ---------------------------------------------------------------- Scanlines

void Scanlines::draw(LvSurface &s) {
  const int p = (int)period;
  for (int y = s.clip_y0; y < s.clip_y1; y++) {
    const int k = y % p;
    const float a = k == 0 ? dark : (k == 1 ? dark * 0.5f : 0.0f);
    if (a <= 0) {
      continue;
    }
    for (int x = s.clip_x0; x < s.clip_x1; x++) {
      s.blend(x, y, Rgb{0, 0, 0}, a);
    }
  }
}

// ---------------------------------------------------------------- LVGL helpers

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, Rgb color, float cx, float cy, float w, Align align) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, to_lv(color), 0);
  lv_obj_set_style_text_align(l, align == Align::Left ? LV_TEXT_ALIGN_LEFT
                                  : align == Align::Right ? LV_TEXT_ALIGN_RIGHT
                                                          : LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  const float x = align == Align::Left ? cx : (align == Align::Center ? cx - w * 0.5f : cx - w);
  lv_obj_set_pos(l, (lv_coord_t)lroundf(x), (lv_coord_t)lroundf(cy - font->line_height * 0.5f));
  lv_obj_set_size(l, (lv_coord_t)lroundf(w), font->line_height);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_label_set_text(l, "");
  return l;
}

void set_text(lv_obj_t *lbl, const char *s) {
  if (strcmp(lv_label_get_text(lbl), s) != 0) {
    lv_label_set_text(lbl, s);
  }
}

void set_color(lv_obj_t *lbl, Rgb c) {
  const lv_color_t n = to_lv(c);
  if (lv_obj_get_style_text_color(lbl, 0).full != n.full) {
    lv_obj_set_style_text_color(lbl, n, 0);
  }
}

lv_obj_t *rect(lv_obj_t *parent, float x, float y, float w, float h, Rgb c, float radius, float opa) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, (lv_coord_t)lroundf(x), (lv_coord_t)lroundf(y));
  lv_obj_set_size(o, (lv_coord_t)lroundf(w), (lv_coord_t)lroundf(h));
  lv_obj_set_style_bg_color(o, to_lv(c), 0);
  lv_obj_set_style_bg_opa(o, (lv_opa_t)(opa * 255.0f), 0);
  lv_obj_set_style_radius(o, (lv_coord_t)radius, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

void set_bg(lv_obj_t *o, Rgb c) {
  const lv_color_t n = to_lv(c);
  if (lv_obj_get_style_bg_color(o, 0).full != n.full) {
    lv_obj_set_style_bg_color(o, n, 0);
  }
}

void show(lv_obj_t *o, bool visible) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == visible) {
    if (visible) {
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

}  // namespace gauge
