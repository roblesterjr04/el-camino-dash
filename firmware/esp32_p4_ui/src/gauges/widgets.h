#pragma once

/**
 * Live gauge parts. Each Shape is an LVGL object covering the face whose DRAW_MAIN event paints with
 * the anti-aliased primitives in paint.h. A Shape only invalidates the pixels that changed (a needle's
 * old and new footprint, the added or removed span of an arc), so moving parts stay cheap.
 */

#include "paint.h"

namespace gauge {

/** Face geometry: faces are designed on an 800-unit square; this maps units to pixels. */
struct Geo {
  float x0 = 0, y0 = 0;  // top-left of the square, in pixels
  float s = 1;           // pixels per unit
  float X(float u) const { return x0 + u * s; }
  float Y(float v) const { return y0 + v * s; }
  float L(float len) const { return len * s; }
  float cx() const { return X(400); }
  float cy() const { return Y(400); }
};

/** Exponential smoothing toward a target: needles glide between 20 Hz sensor frames. */
struct Smooth {
  float v = 0;
  bool init = false;
  float step(float target, float dt, float rate) {
    if (!init) {
      v = target;
      init = true;
      return v;
    }
    v += (target - v) * (1.0f - expf(-rate * dt));
    return v;
  }
};

class Shape {
 public:
  virtual ~Shape() = default;
  lv_obj_t *obj = nullptr;
  /** Creates the object covering `parent` (which must sit at the screen origin). */
  void attach(lv_obj_t *parent);
  void invalidate(Box b);
  virtual void draw(LvSurface &s) = 0;
};

enum class NeedleStyle : uint8_t {
  Classic,  // tapered red blade with a lighter spine
  Pointer,  // short blade from r_start to the tip (sub-gauge windows)
  Hand,     // clock hand: tapered, cream
  Line,     // plain thin line (Modern / Fallout accents)
};

class Needle : public Shape {
 public:
  float cx = 0, cy = 0;
  float len = 100, tail = 0, r_start = 0;
  float w_base = 10, w_tip = 4;
  Rgb color = hex(0xe52620);
  NeedleStyle style = NeedleStyle::Classic;
  float psi = 0;

  void set(float new_psi);
  Box bbox(float at) const;
  void draw(LvSurface &s) override;
};

/** Chrome pivot cap or screw head, drawn above needles. */
class Hub : public Shape {
 public:
  float cx = 0, cy = 0, r = 20;
  bool screw_slot = false;
  void draw(LvSurface &s) override;
};

/**
 * Filled arc: the span psi0..psi1 filled to `frac`. The fill colour runs from c0 at psi0 to c1 at psi1
 * (fixed along the arc, so growing the fill only repaints the new part).
 */
class ArcFill : public Shape {
 public:
  float cx = 0, cy = 0, r = 100, width = 10;
  float psi0 = -130, psi1 = 130;
  Rgb c0 = hex(0xff7a2f), c1 = hex(0xff7a2f);
  bool round_caps = true;
  bool from_end = false;  // fill from psi1 backwards
  bool tip = false;       // bright dot at the moving end
  float frac = 0;
  /** Override colour (e.g. red when over the limit); repaints the whole fill when it changes. */
  bool use_override = false;
  Rgb override_c = hex(0xff3b30);

  void set(float f);
  void set_override(bool on, Rgb c);
  void draw(LvSurface &s) override;

 private:
  float end_psi(float f) const;
  Box span_box(float fa, float fb) const;
};

/** Warning lamp window (an arc band) that glows when on. */
class Lamp : public Shape {
 public:
  float cx = 0, cy = 0, r0 = 0, r1 = 0, psi0 = 0, psi1 = 0;
  Rgb off = hex(0x2a1a10), on = hex(0xffa53a);
  bool lit = false;
  void set(bool v);
  void draw(LvSurface &s) override;
};

/** Fallout CRT scanlines over everything beneath it. */
class Scanlines : public Shape {
 public:
  float period = 5, dark = 0.30f;
  void draw(LvSurface &s) override;
};

// ---------------------------------------------------------------- LVGL helpers

/** Label whose line box is centred on (cx, cy) with a fixed width, so text changes never move it. */
lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, Rgb color, float cx, float cy, float w, Align align);
/** Set text only if it differs (setting text always repaints the label). */
void set_text(lv_obj_t *lbl, const char *s);
void set_color(lv_obj_t *lbl, Rgb c);
/** Plain filled rectangle (Fallout bar segments, pills). */
lv_obj_t *rect(lv_obj_t *parent, float x, float y, float w, float h, Rgb c, float radius = 0, float opa = 1.0f);
void set_bg(lv_obj_t *o, Rgb c);
void show(lv_obj_t *o, bool visible);

}  // namespace gauge
