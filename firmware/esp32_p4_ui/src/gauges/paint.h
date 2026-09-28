#pragma once

/**
 * Anti-aliased software painter for the gauge faces.
 *
 * Every shape is a signed distance function evaluated per pixel, so edges are smooth at any angle and
 * positions are sub-pixel exact. The same primitives draw into two kinds of surface:
 *   - Canvas888: an off-screen RGB888 buffer used to paint a static dial once (then dithered to RGB565);
 *   - LvSurface: LVGL's draw buffer, from inside a DRAW_MAIN event, for needles and other moving parts.
 *
 * Angles ("psi") are degrees clockwise from 12 o'clock, like a gauge face.
 */

#include <lvgl.h>
#include <math.h>
#include <stdint.h>

namespace gauge {

constexpr float kPi = 3.14159265f;
inline float rad(float deg) { return deg * (kPi / 180.0f); }
inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
/** Vector length (sqrtf; hypotf is a slow library call on the ESP32). */
inline float vlen(float x, float y) { return sqrtf(x * x + y * y); }

/** atan2 approximation (error < 1e-4 rad): textures and gradients only, never for edges. */
inline float fast_atan2(float y, float x) {
  const float ax = fabsf(x), ay = fabsf(y);
  const float mx = fmaxf(ax, ay), mn = fminf(ax, ay);
  if (mx == 0.0f) {
    return 0.0f;
  }
  const float a = mn / mx, s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = 3.14159274f - r;
  return y < 0 ? -r : r;
}

inline float smooth(float e0, float e1, float x) {
  const float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

struct Rgb {
  float r, g, b;
};
constexpr Rgb hex(uint32_t h) { return Rgb{(float)((h >> 16) & 0xFF), (float)((h >> 8) & 0xFF), (float)(h & 0xFF)}; }
inline Rgb mix(Rgb a, Rgb b, float t) { return Rgb{lerpf(a.r, b.r, t), lerpf(a.g, b.g, t), lerpf(a.b, b.b, t)}; }
inline Rgb mul(Rgb a, float k) { return Rgb{clampf(a.r * k, 0, 255), clampf(a.g * k, 0, 255), clampf(a.b * k, 0, 255)}; }
inline lv_color_t to_lv(Rgb c) { return lv_color_make((uint8_t)c.r, (uint8_t)c.g, (uint8_t)c.b); }

/** Point on a circle at angle psi. */
inline float polar_x(float cx, float r, float psi) { return cx + r * sinf(rad(psi)); }
inline float polar_y(float cy, float r, float psi) { return cy - r * cosf(rad(psi)); }

struct Box {
  float x0, y0, x1, y1;
};
inline Box box_circle(float cx, float cy, float r) { return Box{cx - r, cy - r, cx + r, cy + r}; }
inline Box box_union(Box a, Box b) {
  return Box{fminf(a.x0, b.x0), fminf(a.y0, b.y0), fmaxf(a.x1, b.x1), fmaxf(a.y1, b.y1)};
}
inline Box box_grow(Box a, float m) { return Box{a.x0 - m, a.y0 - m, a.x1 + m, a.y1 + m}; }
inline Box box_seg(float x0, float y0, float x1, float y1, float r) {
  return Box{fminf(x0, x1) - r, fminf(y0, y1) - r, fmaxf(x0, x1) + r, fmaxf(y0, y1) + r};
}

// ---------------------------------------------------------------- surfaces

/** Off-screen RGB888 (optionally with alpha) painting target. */
struct Canvas888 {
  uint8_t *px = nullptr;  // 3 bytes per pixel, or 4 with alpha
  int w = 0, h = 0;
  bool alpha = false;
  int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;  // [x0, x1)

  void blend(int x, int y, Rgb c, float a) {
    uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * (alpha ? 4 : 3);
    if (!alpha) {
      p[0] = (uint8_t)(p[0] + (c.r - p[0]) * a);
      p[1] = (uint8_t)(p[1] + (c.g - p[1]) * a);
      p[2] = (uint8_t)(p[2] + (c.b - p[2]) * a);
      return;
    }
    const float da = p[3] / 255.0f;
    const float oa = a + da * (1.0f - a);
    if (oa <= 0.0f) {
      return;
    }
    const float k = da * (1.0f - a);
    p[0] = (uint8_t)clampf((c.r * a + p[0] * k) / oa, 0, 255);
    p[1] = (uint8_t)clampf((c.g * a + p[1] * k) / oa, 0, 255);
    p[2] = (uint8_t)clampf((c.b * a + p[2] * k) / oa, 0, 255);
    p[3] = (uint8_t)(oa * 255.0f);
  }
};

/** LVGL's draw buffer, as handed to a DRAW_MAIN event (see LvSurface::from_event). */
struct LvSurface {
  lv_color_t *buf = nullptr;
  int ax = 0, ay = 0, stride = 0;  // buffer origin (screen coords) and row length
  int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;

  static LvSurface from_event(lv_event_t *e) {
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    LvSurface s;
    s.buf = (lv_color_t *)dc->buf;
    s.ax = dc->buf_area->x1;
    s.ay = dc->buf_area->y1;
    s.stride = lv_area_get_width(dc->buf_area);
    s.clip_x0 = dc->clip_area->x1;
    s.clip_y0 = dc->clip_area->y1;
    s.clip_x1 = dc->clip_area->x2 + 1;
    s.clip_y1 = dc->clip_area->y2 + 1;
    return s;
  }

  void blend(int x, int y, Rgb c, float a) {
    lv_color_t &d = buf[(y - ay) * stride + (x - ax)];
    if (a >= 0.996f) {
      d = to_lv(c);
      return;
    }
    const uint32_t f = lv_color_to32(d);
    const float r = (float)((f >> 16) & 0xFF), g = (float)((f >> 8) & 0xFF), b = (float)(f & 0xFF);
    d = lv_color_make((uint8_t)(r + (c.r - r) * a), (uint8_t)(g + (c.g - g) * a), (uint8_t)(b + (c.b - b) * a));
  }
};

/**
 * Core loop: for each pixel of `b` inside the surface clip, `f(x, y, &color)` returns coverage 0..1
 * (x, y are the pixel centre) and may set the color.
 */
template <class S, class F>
void fill(S &s, Box b, F &&f) {
  const int x0 = (int)fmaxf(floorf(b.x0), (float)s.clip_x0);
  const int y0 = (int)fmaxf(floorf(b.y0), (float)s.clip_y0);
  const int x1 = (int)fminf(ceilf(b.x1), (float)s.clip_x1);
  const int y1 = (int)fminf(ceilf(b.y1), (float)s.clip_y1);
  for (int y = y0; y < y1; y++) {
    const float py = (float)y + 0.5f;
    for (int x = x0; x < x1; x++) {
      Rgb c{0, 0, 0};
      const float a = f((float)x + 0.5f, py, c);
      if (a > 0.002f) {
        s.blend(x, y, c, a > 1.0f ? 1.0f : a);
      }
    }
  }
}

/**
 * Like fill(), but only visits pixels within rmin..rmax of (cx, cy) (plus a 1.5 px margin), row span
 * by row span: rings and discs skip the empty inside and corners of their bounding box.
 */
template <class S, class F>
void fill_annulus(S &s, Box b, float cx, float cy, float rmin, float rmax, F &&f) {
  const float ro = rmax + 1.5f, ri = rmin - 1.5f;
  const int bx0 = (int)fmaxf(floorf(fmaxf(b.x0, cx - ro)), (float)s.clip_x0);
  const int bx1 = (int)fminf(ceilf(fminf(b.x1, cx + ro)), (float)s.clip_x1);
  const int y0 = (int)fmaxf(floorf(fmaxf(b.y0, cy - ro)), (float)s.clip_y0);
  const int y1 = (int)fminf(ceilf(fminf(b.y1, cy + ro)), (float)s.clip_y1);
  for (int y = y0; y < y1; y++) {
    const float py = (float)y + 0.5f, dy = py - cy;
    const float xo2 = ro * ro - dy * dy;
    if (xo2 <= 0.0f) {
      continue;
    }
    const float xo = sqrtf(xo2);
    const float xi2 = ri > 0.0f ? ri * ri - dy * dy : -1.0f;
    const float xi = xi2 > 0.0f ? sqrtf(xi2) : -1.0f;
    // Up to two spans: [cx - xo, cx - xi] and [cx + xi, cx + xo]; one when there is no hole.
    for (int k = 0; k < 2; k++) {
      float a, e;
      if (xi < 0.0f) {
        if (k) break;
        a = cx - xo;
        e = cx + xo;
      } else {
        a = k ? cx + xi : cx - xo;
        e = k ? cx + xo : cx - xi;
      }
      const int xa = (int)fmaxf(floorf(a), (float)bx0), xe = (int)fminf(ceilf(e), (float)bx1);
      for (int x = xa; x < xe; x++) {
        Rgb c{0, 0, 0};
        const float al = f((float)x + 0.5f, py, c);
        if (al > 0.002f) {
          s.blend(x, y, c, al > 1.0f ? 1.0f : al);
        }
      }
    }
  }
}

/**
 * Like fill(), but only visits pixels within r of the segment a→b, row span by row span. A diagonal
 * needle covers a small fraction of its bounding box; this skips the rest.
 */
template <class S, class F>
void fill_along(S &s, float ax, float ay, float bx, float by, float r, F &&f) {
  const float rr = r + 1.5f;
  const int y0 = (int)fmaxf(floorf(fminf(ay, by) - rr), (float)s.clip_y0);
  const int y1 = (int)fminf(ceilf(fmaxf(ay, by) + rr), (float)s.clip_y1);
  const float dy = by - ay;
  for (int y = y0; y < y1; y++) {
    const float py = (float)y + 0.5f;
    float xa, xb;
    if (fabsf(dy) < 1e-3f) {
      xa = fminf(ax, bx);
      xb = fmaxf(ax, bx);
    } else {
      float t0 = (py - rr - ay) / dy, t1 = (py + rr - ay) / dy;
      if (t0 > t1) {
        const float t = t0;
        t0 = t1;
        t1 = t;
      }
      t0 = fmaxf(t0, 0.0f);
      t1 = fminf(t1, 1.0f);
      if (t0 > t1) {
        continue;
      }
      const float x0 = ax + (bx - ax) * t0, x1 = ax + (bx - ax) * t1;
      xa = fminf(x0, x1);
      xb = fmaxf(x0, x1);
    }
    const int xs = (int)fmaxf(floorf(xa - rr), (float)s.clip_x0);
    const int xe = (int)fminf(ceilf(xb + rr), (float)s.clip_x1);
    for (int x = xs; x < xe; x++) {
      Rgb c{0, 0, 0};
      const float al = f((float)x + 0.5f, py, c);
      if (al > 0.002f) {
        s.blend(x, y, c, al > 1.0f ? 1.0f : al);
      }
    }
  }
}

/** Coverage of a shape from its signed distance (negative inside), 1 px anti-aliasing. */
inline float cover(float d) { return clampf(0.5f - d, 0.0f, 1.0f); }

// ---------------------------------------------------------------- distance functions

inline float sd_circle(float px, float py, float cx, float cy, float r) { return vlen(px - cx, py - cy) - r; }

inline float sd_segment(float px, float py, float ax, float ay, float bx, float by) {
  const float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
  const float l2 = bax * bax + bay * bay;
  const float h = l2 > 0 ? clampf((pax * bax + pay * bay) / l2, 0.0f, 1.0f) : 0.0f;
  return vlen(pax - bax * h, pay - bay * h);
}

/** Box of half-length hl along (ux, uy) and half-width hw, centred at (cx, cy). */
inline float sd_obox(float px, float py, float cx, float cy, float ux, float uy, float hl, float hw) {
  const float dx = px - cx, dy = py - cy;
  const float u = fabsf(dx * ux + dy * uy) - hl;
  const float v = fabsf(-dx * uy + dy * ux) - hw;
  return vlen(fmaxf(u, 0.0f), fmaxf(v, 0.0f)) + fminf(fmaxf(u, v), 0.0f);
}

inline float sd_round_rect(float px, float py, float x, float y, float w, float h, float r) {
  const float qx = fabsf(px - (x + w * 0.5f)) - (w * 0.5f - r);
  const float qy = fabsf(py - (y + h * 0.5f)) - (h * 0.5f - r);
  return vlen(fmaxf(qx, 0.0f), fmaxf(qy, 0.0f)) + fminf(fmaxf(qx, qy), 0.0f) - r;
}

/** Tapered capsule from a (radius ra) to b (radius rb). */
inline float sd_uneven_capsule(float px, float py, float ax, float ay, float ra, float bx, float by, float rb) {
  // iq's 2D uneven capsule, in the frame where a is the origin and b lies on +y.
  const float dx = bx - ax, dy = by - ay;
  const float h = vlen(dx, dy);
  if (h < 1e-3f) {
    return sd_circle(px, py, ax, ay, fmaxf(ra, rb));
  }
  const float ux = dx / h, uy = dy / h;
  const float qx0 = px - ax, qy0 = py - ay;
  float qx = fabsf(qx0 * uy - qy0 * ux);  // across
  const float qy = qx0 * ux + qy0 * uy;   // along
  const float b = (ra - rb) / h;
  const float a = sqrtf(fmaxf(1.0f - b * b, 0.0f));
  const float k = -qx * b + qy * a;  // dot(q, (-b, a))
  if (k < 0.0f) {
    return vlen(qx, qy) - ra;
  }
  if (k > a * h) {
    return vlen(qx, qy - h) - rb;
  }
  return qx * a + qy * b - ra;
}

/** Angle of a point around (cx, cy), degrees clockwise from 12 o'clock (approximate: shading only). */
inline float angle_of(float px, float py, float cx, float cy) {
  return fast_atan2(px - cx, cy - py) * (180.0f / kPi);
}
inline float wrap180(float a) {
  while (a > 180.0f) a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

/**
 * Band between radii r0..r1 spanning psi0..psi1 (psi1 > psi0). round_caps: semicircular ends;
 * otherwise square ends along the radius. Constants are precomputed; the per-pixel test is exact and
 * uses no trigonometry.
 */
struct ArcBand {
  float cx, cy, rm, hw, cos_half;
  bool full, round;
  float mx, my;                        // unit direction of the middle of the span
  float e0x, e0y, e1x, e1y;            // end centres on the mid radius
  float u0x, u0y, u1x, u1y;            // radial directions at the ends
  ArcBand(float cx_, float cy_, float r0, float r1, float psi0, float psi1, bool round_caps)
      : cx(cx_), cy(cy_), rm((r0 + r1) * 0.5f), hw((r1 - r0) * 0.5f), round(round_caps) {
    const float half = (psi1 - psi0) * 0.5f, mid = (psi0 + psi1) * 0.5f;
    full = half >= 179.99f;
    cos_half = cosf(rad(half));
    mx = sinf(rad(mid));
    my = -cosf(rad(mid));
    u0x = sinf(rad(psi0));
    u0y = -cosf(rad(psi0));
    u1x = sinf(rad(psi1));
    u1y = -cosf(rad(psi1));
    e0x = cx + u0x * rm;
    e0y = cy + u0y * rm;
    e1x = cx + u1x * rm;
    e1y = cy + u1y * rm;
  }
  float sd(float px, float py) const {
    const float dx = px - cx, dy = py - cy;
    const float rho = vlen(dx, dy);
    if (full || dx * mx + dy * my >= rho * cos_half) {
      return fabsf(rho - rm) - hw;
    }
    const bool at_end = mx * dy - my * dx > 0.0f;  // clockwise of the middle → the psi1 end
    const float ex = at_end ? e1x : e0x, ey = at_end ? e1y : e0y;
    if (round) {
      return vlen(px - ex, py - ey) - hw;
    }
    const float ux = at_end ? u1x : u0x, uy = at_end ? u1y : u0y;
    const float ddx = px - ex, ddy = py - ey;
    const float u = fabsf(ddx * ux + ddy * uy) - hw;
    const float v = fabsf(-ddx * uy + ddy * ux);  // along the tangent
    return vlen(fmaxf(u, 0.0f), fmaxf(v, 0.0f)) + fminf(fmaxf(u, v), 0.0f);
  }
};

inline Box box_arc_band(float cx, float cy, float r0, float r1, float psi0, float psi1) {
  // Conservative: end points plus any axis extremes crossed.
  Box b = box_union(box_circle(polar_x(cx, r0, psi0), polar_y(cy, r0, psi0), 0),
                    box_circle(polar_x(cx, r1, psi0), polar_y(cy, r1, psi0), 0));
  b = box_union(b, box_circle(polar_x(cx, r0, psi1), polar_y(cy, r0, psi1), 0));
  b = box_union(b, box_circle(polar_x(cx, r1, psi1), polar_y(cy, r1, psi1), 0));
  for (int k = -720; k <= 720; k += 90) {
    if (k > psi0 && k < psi1) {
      b = box_union(b, box_circle(polar_x(cx, r1, (float)k), polar_y(cy, r1, (float)k), 0));
    }
  }
  const float hw = (r1 - r0) * 0.5f;
  return box_grow(b, hw + 2.0f);
}

// ---------------------------------------------------------------- shading helpers

/** Cheap integer hash → 0..1. */
inline float hash01(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return (float)(x & 0xFFFFFF) / 16777215.0f;
}
/** Smooth 1D value noise. */
inline float noise1(float t, uint32_t seed) {
  const float f = floorf(t);
  const float u = t - f;
  const uint32_t i = (uint32_t)(int32_t)f;
  const float a = hash01(i * 747796405u + seed), b = hash01((i + 1u) * 747796405u + seed);
  return lerpf(a, b, u * u * (3.0f - 2.0f * u));
}

/** Spun (conical) brushed aluminium, like a machined dash cap. */
inline Rgb brushed_metal(float px, float py, float cx, float cy, float r, float light_psi = -35.0f) {
  const float dx = px - cx, dy = py - cy;
  const float r2 = dx * dx + dy * dy;
  const float rho = sqrtf(r2) / r;
  const float th = fast_atan2(dy, dx);
  // cos/sin of 2(θ - light) and 4(θ - light) straight from dx, dy (no trig per pixel).
  const float inv = r2 > 0.0f ? 1.0f / r2 : 0.0f;
  const float c2 = (dx * dx - dy * dy) * inv, s2 = 2.0f * dx * dy * inv;
  const float l = rad(light_psi + 90.0f);  // light direction in the atan2 frame
  const float cl = cosf(2.0f * l), sl = sinf(2.0f * l);
  const float ca = c2 * cl + s2 * sl, sa = s2 * cl - c2 * sl;  // cos, sin of 2(θ - l)
  const float c4 = 2.0f * ca * ca - 1.0f, s4 = 2.0f * sa * ca;
  float v = 0.60f + 0.20f * ca + 0.06f * (c4 * 0.764842f - s4 * 0.644218f);  // cos(4a + 0.7)
  v += 0.07f * (noise1(th * 160.0f, 17u) - 0.5f) + 0.05f * (noise1(th * 900.0f, 91u) - 0.5f);
  v += 0.03f * (noise1(rho * 60.0f, 7u) - 0.5f);  // faint concentric turning marks
  v *= 1.0f - 0.10f * rho * rho;                  // slightly darker toward the rim
  return mix(hex(0x5c6166), hex(0xf4f6f8), clampf(v, 0.0f, 1.0f));
}

/** Polished chrome band between r0..r1: bright/dark sweep around, rolled bevel across. */
inline Rgb chrome_band(float px, float py, float cx, float cy, float r0, float r1) {
  const float dx = px - cx, dy = py - cy;
  const float rho = vlen(dx, dy);
  const float t = clampf((rho - r0) / fmaxf(r1 - r0, 1.0f), 0.0f, 1.0f);
  const float th = fast_atan2(dy, dx);
  const float sweep = 0.5f + 0.5f * cosf(2.0f * (th + 2.2f));
  const float bevel = 4.0f * t * (1.0f - t);  // ~sin(t·π)
  const float b2 = bevel * bevel;
  float v = 0.18f + 0.62f * sweep * (0.55f + 0.45f * bevel) + 0.25f * b2 * b2 * b2;
  v += 0.05f * (noise1(th * 40.0f, 3u) - 0.5f);
  return mix(hex(0x2a2d31), hex(0xffffff), clampf(v, 0.0f, 1.0f));
}

/** Chrome ball (pivot caps, screws) lit from the upper left. */
inline Rgb chrome_sphere(float px, float py, float cx, float cy, float r) {
  const float nx = (px - cx) / r, ny = (py - cy) / r;
  const float nn = nx * nx + ny * ny;
  const float nz = sqrtf(fmaxf(1.0f - nn, 0.0f));
  // Environment: bright sky above the horizon, dark below; the horizon bends with the surface.
  const float horizon = ny + 0.25f * nz;
  Rgb env = horizon < 0.0f ? mix(hex(0xffffff), hex(0x9aa3ab), clampf(-horizon * 1.2f, 0, 1))
                           : mix(hex(0x3a3f45), hex(0x0e1012), clampf(horizon * 1.6f, 0, 1));
  const float lx = -0.45f, ly = -0.55f, lz = 0.70f;
  const float d = clampf(nx * lx + ny * ly + nz * lz, 0.0f, 1.0f);
  const float d2 = d * d, d4 = d2 * d2, d8 = d4 * d4, d32 = d8 * d8 * d8 * d8;
  const float spec = d32 * d8;  // d^40
  env = mix(env, hex(0xffffff), spec);
  return mix(env, hex(0x6c737a), 0.35f * nn * nn);  // soften the rim
}

// ---------------------------------------------------------------- shape helpers

template <class S>
void disc(S &s, float cx, float cy, float r, Rgb c, float opa = 1.0f) {
  fill_annulus(s, box_circle(cx, cy, r + 1), cx, cy, 0.0f, r, [&](float x, float y, Rgb &o) {
    o = c;
    return cover(sd_circle(x, y, cx, cy, r)) * opa;
  });
}

template <class S, class Sh>
void disc_shaded(S &s, float cx, float cy, float r, Sh &&shade) {
  fill_annulus(s, box_circle(cx, cy, r + 1), cx, cy, 0.0f, r, [&](float x, float y, Rgb &o) {
    const float a = cover(sd_circle(x, y, cx, cy, r));
    if (a > 0) {
      o = shade(x, y);
    }
    return a;
  });
}

template <class S>
void ring(S &s, float cx, float cy, float r0, float r1, Rgb c, float opa = 1.0f) {
  const float rm = (r0 + r1) * 0.5f, hw = (r1 - r0) * 0.5f;
  fill_annulus(s, box_circle(cx, cy, r1 + 1), cx, cy, r0, r1, [&](float x, float y, Rgb &o) {
    o = c;
    return cover(fabsf(vlen(x - cx, y - cy) - rm) - hw) * opa;
  });
}

template <class S, class Sh>
void ring_shaded(S &s, float cx, float cy, float r0, float r1, Sh &&shade) {
  const float rm = (r0 + r1) * 0.5f, hw = (r1 - r0) * 0.5f;
  fill_annulus(s, box_circle(cx, cy, r1 + 1), cx, cy, r0, r1, [&](float x, float y, Rgb &o) {
    const float d = vlen(x - cx, y - cy);
    const float a = cover(fabsf(d - rm) - hw);
    if (a > 0) {
      o = shade(x, y);
    }
    return a;
  });
}

template <class S>
void seg(S &s, float x0, float y0, float x1, float y1, float w, Rgb c, float opa = 1.0f) {
  const float r = w * 0.5f;
  fill_along(s, x0, y0, x1, y1, r, [&](float x, float y, Rgb &o) {
    o = c;
    return cover(sd_segment(x, y, x0, y0, x1, y1) - r) * opa;
  });
}

/** Square-ended bar from (x0, y0) to (x1, y1). */
template <class S>
void bar(S &s, float x0, float y0, float x1, float y1, float w, Rgb c, float opa = 1.0f) {
  const float len = vlen(x1 - x0, y1 - y0);
  if (len < 1e-3f) {
    return;
  }
  const float ux = (x1 - x0) / len, uy = (y1 - y0) / len;
  const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
  fill_along(s, x0, y0, x1, y1, w * 0.75f, [&](float x, float y, Rgb &o) {
    o = c;
    return cover(sd_obox(x, y, mx, my, ux, uy, len * 0.5f, w * 0.5f)) * opa;
  });
}

/** Radial tick from r0 to r1 at angle psi. */
template <class S>
void tick(S &s, float cx, float cy, float psi, float r0, float r1, float w, Rgb c, bool round = false) {
  const float x0 = polar_x(cx, r0, psi), y0 = polar_y(cy, r0, psi);
  const float x1 = polar_x(cx, r1, psi), y1 = polar_y(cy, r1, psi);
  if (round) {
    seg(s, x0, y0, x1, y1, w, c);
  } else {
    bar(s, x0, y0, x1, y1, w, c);
  }
}

template <class S>
void arc_band(S &s, float cx, float cy, float r0, float r1, float psi0, float psi1, Rgb c, bool round_caps,
              float opa = 1.0f) {
  if (psi1 - psi0 < 0.05f) {
    return;
  }
  const ArcBand band(cx, cy, r0, r1, psi0, psi1, round_caps);
  fill_annulus(s, box_arc_band(cx, cy, r0, r1, psi0, psi1), cx, cy, r0, r1, [&](float x, float y, Rgb &o) {
    o = c;
    return cover(band.sd(x, y)) * opa;
  });
}

template <class S, class Sh>
void arc_band_shaded(S &s, float cx, float cy, float r0, float r1, float psi0, float psi1, bool round_caps,
                     Sh &&shade) {
  if (psi1 - psi0 < 0.05f) {
    return;
  }
  const ArcBand band(cx, cy, r0, r1, psi0, psi1, round_caps);
  fill_annulus(s, box_arc_band(cx, cy, r0, r1, psi0, psi1), cx, cy, r0, r1, [&](float x, float y, Rgb &o) {
    const float a = cover(band.sd(x, y));
    if (a > 0) {
      o = shade(x, y);
    }
    return a;
  });
}

template <class S>
void round_rect(S &s, float x, float y, float w, float h, float r, Rgb c, float opa = 1.0f) {
  fill(s, Box{x - 1, y - 1, x + w + 1, y + h + 1}, [&](float px, float py, Rgb &o) {
    o = c;
    return cover(sd_round_rect(px, py, x, y, w, h, r)) * opa;
  });
}

template <class S>
void round_rect_ring(S &s, float x, float y, float w, float h, float r, float t, Rgb c, float opa = 1.0f) {
  fill(s, Box{x - 1, y - 1, x + w + 1, y + h + 1}, [&](float px, float py, Rgb &o) {
    o = c;
    return cover(fabsf(sd_round_rect(px, py, x, y, w, h, r) + t * 0.5f) - t * 0.5f) * opa;
  });
}

// ---------------------------------------------------------------- text

enum class Align : uint8_t { Left, Center, Right };

/** Advance width of a UTF-8 string, including kerning. */
float text_width(const lv_font_t *font, const char *s);

/**
 * Draw a UTF-8 string. (x, y) is the anchor: horizontal per `align`, vertically the middle of the
 * font's line box (same convention as the label helpers). `angle` rotates the text about the anchor
 * (degrees clockwise).
 */
void text(Canvas888 &s, const lv_font_t *font, const char *str, float x, float y, Align align, Rgb c,
          float angle = 0.0f, float opa = 1.0f);

/**
 * Text along a circle of radius r (the middle of the line box sits on the circle) centred at psi.
 * top=true: reads clockwise over the top, letters upright outward; false: reads counter-clockwise
 * along the bottom, letters upright.
 */
void text_arc(Canvas888 &s, const lv_font_t *font, const char *str, float cx, float cy, float r, float psi,
              bool top, Rgb c, float letter_spacing = 0.0f);

/** Convert the painted RGB888 buffer to RGB565 with ordered dithering (gradients without banding). */
void to_rgb565(const Canvas888 &src, uint16_t *dst);

/** Convert an RGBA painting to LVGL's TRUE_COLOR_ALPHA layout (RGB565 + alpha, 3 bytes per pixel). */
void to_rgb565a8(const Canvas888 &src, uint8_t *dst);

}  // namespace gauge
