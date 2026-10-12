// Classic theme: black glass dials, cream numerals, polished chrome rings, spun-aluminium caps and red
// needles. Everything here is drawn; the layouts follow the original Classic dial art.
// The painter runs per pixel: always build it optimised, even in -Os firmware builds.
#pragma GCC optimize("O3", "fast-math")

#include <stdio.h>

#include "face.h"

namespace gauge {

namespace {

const Rgb kInk = hex(0xf2eee4);
const Rgb kInkDim = hex(0xb7b2a6);
const Rgb kTick = hex(0xe8e5dc);
const Rgb kTickMinor = hex(0x8e8b85);
const Rgb kCyan = hex(0xd4eef2);
const Rgb kRed = hex(0xe8392b);
const Rgb kNeedle = hex(0xe3241c);
const Rgb kAmber = hex(0xe8a33d);
const Rgb kEngrave = hex(0x40444a);

#define IC_MUSIC "\xEE\x90\x85"

// ---------------------------------------------------------------- shared dial parts

/** Glossy black dial: dark vignette and a faint sheen from the upper left. */
void dial_bg(Canvas888 &c, const Geo &g) {
  const float cx = g.cx(), cy = g.cy(), R = g.L(400);
  // Vignette by distance and a Gaussian sheen (separable: one exp per row and per column).
  static float lut[257], ex[1024], ey[1024];
  for (int i = 0; i <= 256; i++) {
    lut[i] = clampf(powf((float)i / 128.0f, 1.3f), 0, 1);
  }
  for (int i = 0; i < c.w && i < 1024; i++) {
    const float sx = ((float)i + 0.5f - (cx - 0.38f * R)) / R;
    ex[i] = expf(-sx * sx * 3.2f);
  }
  for (int i = 0; i < c.h && i < 1024; i++) {
    const float sy = ((float)i + 0.5f - (cy - 0.48f * R)) / R;
    ey[i] = expf(-sy * sy * 3.2f);
  }
  const Rgb in = hex(0x1c1d20), out = hex(0x050506), sheen = hex(0x767a82);
  fill(c, Box{0, 0, (float)c.w, (float)c.h}, [&](float x, float y, Rgb &o) {
    const float d = vlen(x - cx, y - cy) / R;
    const int k = (int)(d * 128.0f);
    o = mix(mix(in, out, lut[k < 256 ? k : 256]), sheen, ex[(int)x & 1023] * ey[(int)y & 1023] * 0.10f);
    return 1.0f;
  });
}

void chrome_ring(Canvas888 &c, const Geo &g, float r0, float r1) {
  const float cx = g.cx(), cy = g.cy();
  ring_shaded(c, cx, cy, g.L(r0), g.L(r1), [&](float x, float y) { return chrome_band(x, y, cx, cy, g.L(r0), g.L(r1)); });
  ring(c, cx, cy, g.L(r0) - 1.2f, g.L(r0), hex(0x000000), 0.6f);
  ring(c, cx, cy, g.L(r1), g.L(r1) + 1.2f, hex(0x000000), 0.6f);
}

/** Spun-aluminium cap with a drop shadow and a bevelled edge. */
void brushed_cap(Canvas888 &c, float cx, float cy, float r, float shadow) {
  fill_annulus(c, box_circle(cx + shadow * 0.3f, cy + shadow * 0.5f, r + shadow + 2), cx + shadow * 0.3f,
               cy + shadow * 0.5f, r - shadow, r + shadow, [&](float x, float y, Rgb &o) {
    o = Rgb{0, 0, 0};
    const float d = sd_circle(x, y, cx + shadow * 0.3f, cy + shadow * 0.5f, r);
    return 0.65f * (1.0f - smooth(-2.0f, shadow, d));
  });
  disc_shaded(c, cx, cy, r, [&](float x, float y) { return brushed_metal(x, y, cx, cy, r); });
  const float bw = fmaxf(2.0f, r * 0.03f);
  ring_shaded(c, cx, cy, r - bw, r, [&](float x, float y) {
    const float a = fast_atan2(y - cy, x - cx);
    const float v = 0.5f + 0.5f * cosf(a + 2.3f);  // bright toward the upper left
    return mix(hex(0x4a4f55), hex(0xffffff), v);
  });
}

void ball(Canvas888 &c, float cx, float cy, float r) {
  disc_shaded(c, cx, cy, r, [&](float x, float y) { return chrome_sphere(x, y, cx, cy, r); });
}

void screw(Canvas888 &c, float cx, float cy, float r) {
  fill_annulus(c, box_circle(cx + 1.5f, cy + 2.5f, r + 5), cx + 1.5f, cy + 2.5f, 0.0f, r + 4, [&](float x, float y, Rgb &o) {
    o = Rgb{0, 0, 0};
    return 0.55f * (1.0f - smooth(-1.5f, 4.0f, sd_circle(x, y, cx + 1.5f, cy + 2.5f, r)));
  });
  ball(c, cx, cy, r);
  const float ux = cosf(rad(28)), uy = sinf(rad(28));
  bar(c, cx - ux * r * 0.78f, cy - uy * r * 0.78f, cx + ux * r * 0.78f, cy + uy * r * 0.78f, fmaxf(1.5f, r * 0.2f),
      hex(0x1c1e21), 0.9f);
}

/** Black window with a thin chrome rim (odometer, miles-to-empty). */
void window(Canvas888 &c, float x, float y, float w, float h, float r) {
  round_rect(c, x - 2, y - 2, w + 4, h + 4, r + 2, hex(0xc9ced3), 0.85f);
  round_rect(c, x, y, w, h, r, hex(0x030303));
  round_rect_ring(c, x, y, w, h, r, 3, hex(0x000000), 0.5f);
}

void numeral(Canvas888 &c, const Geo &g, const lv_font_t *f, const char *s, float r, float psi, Rgb col) {
  text(c, f, s, polar_x(g.cx(), g.L(r), psi), polar_y(g.cy(), g.L(r), psi), Align::Center, col);
}

Needle *classic_needle(Face *, Needle *n, const Geo &g, float cx, float cy, float len, float tail, float wb, float wt) {
  n->cx = cx;
  n->cy = cy;
  n->len = g.L(len);
  n->tail = g.L(tail);
  n->w_base = g.L(wb);
  n->w_tip = g.L(wt);
  n->color = kNeedle;
  n->style = NeedleStyle::Classic;
  return n;
}

Hub *hub_at(Hub *h, float cx, float cy, float r) {
  h->cx = cx;
  h->cy = cy;
  h->r = r;
  return h;
}

// ---------------------------------------------------------------- speedometer

class ClassicSpeedo : public Face {
 public:
  static float psi_for(float mph) { return -123.0f + 246.0f * clampf(mph, 0.0f, 125.0f) / 120.0f; }

  void paint(Canvas888 &c, const Geo &g) override {
    const float cx = g.cx(), cy = g.cy();
    dial_bg(c, g);
    for (int v = 0; v <= 120; v += 5) {
      const bool major = v % 10 == 0;
      tick(c, cx, cy, psi_for((float)v), g.L(major ? 234 : 252), g.L(273), g.L(major ? 6.5f : 3.5f),
           major ? kCyan : mul(kCyan, 0.72f));
    }
    ring(c, cx, cy, g.L(278), g.L(280.5f), kTick);
    ring(c, cx, cy, g.L(285), g.L(290), kTick);
    char b[8];
    for (int v = 0; v <= 120; v += 10) {
      snprintf(b, sizeof(b), "%d", v);
      numeral(c, g, &elc_bc_58, b, 340, psi_for((float)v), kInk);
    }
    brushed_cap(c, cx, cy, g.L(206), g.L(16));
    screw(c, g.X(318), g.Y(334), g.L(15));
    screw(c, g.X(482), g.Y(334), g.L(15));
    window(c, g.X(270), g.Y(452), g.L(260), g.L(64), g.L(10));
    text(c, &elc_bc_34, "MPH", g.X(400), g.Y(560), Align::Center, kEngrave);
  }

  void build(lv_obj_t *root, const Geo &g) override {
    odo_ = label(root, &elc_bc_46, kInk, g.X(400), g.Y(485), g.L(248), Align::Center);
    needle_ = classic_needle(this, add<Needle>(root), g, g.cx(), g.cy(), 268, 50, 15, 5);
    hub_at(add<Hub>(root), g.cx(), g.cy(), g.L(28));
  }

  void update(const GaugeData &d, float dt) override {
    needle_->set(psi_for(mph_.step(d.fresh ? d.mph : 0.0f, dt, 8.0f)));
    char b[40];
    if (d.banner && d.banner[0]) {
      snprintf(b, sizeof(b), "%s", d.banner);
      set_color(odo_, kAmber);
    } else {
      if (d.odo_valid) {
        snprintf(b, sizeof(b), "%.1f", (double)d.odo_mi);
      } else {
        snprintf(b, sizeof(b), "------");
      }
      set_color(odo_, kInk);
    }
    set_text(odo_, b);
  }

 private:
  Needle *needle_ = nullptr;
  lv_obj_t *odo_ = nullptr;
  Smooth mph_;
};

// ---------------------------------------------------------------- tachometer

class ClassicTach : public Face {
 public:
  static float psi_for(float rpm) { return -110.0f + 220.0f * clampf(rpm, 0.0f, 6000.0f) / 6000.0f; }

  void paint(Canvas888 &c, const Geo &g) override {
    const float cx = g.cx(), cy = g.cy();
    dial_bg(c, g);
    arc_band(c, cx, cy, g.L(250), g.L(258), psi_for(5000), psi_for(6000), kRed, false);
    for (int k = 0; k <= 12; k++) {
      const float rpm = (float)k * 500.0f;
      const bool major = k % 2 == 0, red = rpm >= 5000.0f;
      tick(c, cx, cy, psi_for(rpm), g.L(major ? 212 : 230), g.L(258), g.L(major ? 6.5f : 3.5f),
           red ? kRed : (major ? kTick : kTickMinor));
    }
    chrome_ring(c, g, 266, 278);
    char b[8];
    for (int v = 0; v <= 60; v += 10) {
      snprintf(b, sizeof(b), "%d", v);
      numeral(c, g, &elc_bc_64d, b, 332, psi_for((float)v * 100.0f), v >= 50 ? hex(0xff6a5c) : kInk);
    }
    text_arc(c, &elc_bc_34, "RPM", cx, cy, g.L(334), 214, false, kInkDim, g.L(3));
    text_arc(c, &elc_bc_34, "x100", cx, cy, g.L(334), 146, false, kInkDim, g.L(3));
    brushed_cap(c, cx, cy, g.L(158), g.L(14));
    text(c, &elc_bc_34, "TACH", g.X(400), g.Y(596), Align::Center, kInkDim);
  }

  void build(lv_obj_t *root, const Geo &g) override {
    needle_ = classic_needle(this, add<Needle>(root), g, g.cx(), g.cy(), 258, 46, 15, 5);
    hub_at(add<Hub>(root), g.cx(), g.cy(), g.L(26));
  }

  void update(const GaugeData &d, float dt) override {
    needle_->set(psi_for(rpm_.step(d.fresh ? d.rpm : 0.0f, dt, 10.0f)));
  }

 private:
  Needle *needle_ = nullptr;
  Smooth rpm_;
};

// ---------------------------------------------------------------- clock

class ClassicClock : public Face {
 public:
  bool uses_sensors() const override { return false; }

  void paint(Canvas888 &c, const Geo &g) override {
    const float cx = g.cx(), cy = g.cy();
    dial_bg(c, g);
    for (int m = 0; m < 60; m++) {
      if (m % 5 != 0) {
        tick(c, cx, cy, (float)m * 6.0f, g.L(340), g.L(352), g.L(2.5f), kTickMinor);
      }
    }
    for (int h = 1; h <= 12; h++) {
      if (h % 3 != 0) {
        tick(c, cx, cy, (float)h * 30.0f, g.L(312), g.L(352), g.L(8), kTick, true);
      }
    }
    numeral(c, g, &elc_bc_64d, "12", 330, 0, kInk);
    numeral(c, g, &elc_bc_64d, "3", 334, 90, kInk);
    numeral(c, g, &elc_bc_64d, "6", 330, 180, kInk);
    numeral(c, g, &elc_bc_64d, "9", 334, 270, kInk);
    ring(c, cx, cy, g.L(291), g.L(298), kTick);
    brushed_cap(c, cx, cy, g.L(284), g.L(10));
  }

  void build(lv_obj_t *root, const Geo &g) override {
    date_ = label(root, &elc_bc_34, kEngrave, g.X(400), g.Y(292), g.L(380), Align::Center);
    place_ = label(root, &elc_b_22, kEngrave, g.X(400), g.Y(532), g.L(400), Align::Center);
    hour_ = classic_needle(this, add<Needle>(root), g, g.cx(), g.cy(), 160, 34, 18, 8);
    min_ = classic_needle(this, add<Needle>(root), g, g.cx(), g.cy(), 262, 40, 14, 5);
    hub_at(add<Hub>(root), g.cx(), g.cy(), g.L(24));
  }

  void update(const GaugeData &d, float) override {
    const float m = (float)d.minute + d.second / 60.0f;
    min_->set(m * 6.0f);
    hour_->set((float)(d.hour % 12) * 30.0f + m * 0.5f);
    set_text(date_, d.time_valid && d.date_text ? d.date_text : "");
    set_text(place_, d.place_text ? d.place_text : "");
  }

 private:
  Needle *hour_ = nullptr, *min_ = nullptr;
  lv_obj_t *date_ = nullptr, *place_ = nullptr;
};

// ---------------------------------------------------------------- fuel economy

class ClassicEcon : public Face {
 public:
  static float psi_avg(float v) { return -64.0f + 128.0f * clampf(v, 0.0f, 40.0f) / 40.0f; }
  static float psi_inst(float v) { return -50.0f + 100.0f * clampf(v, 0.0f, 40.0f) / 40.0f; }

  void paint(Canvas888 &c, const Geo &g) override {
    const float cx = g.cx(), cy = g.cy();
    dial_bg(c, g);
    for (int v = 0; v <= 40; v += 2) {
      const bool major = v % 10 == 0;
      tick(c, cx, cy, psi_avg((float)v), g.L(major ? 262 : 278), g.L(298), g.L(major ? 6.5f : 3.2f),
           major ? kTick : kTickMinor);
    }
    char b[8];
    for (int v = 0; v <= 40; v += 10) {
      snprintf(b, sizeof(b), "%d", v);
      numeral(c, g, &elc_bc_64d, b, 340, psi_avg((float)v), kInk);
    }
    chrome_ring(c, g, 244, 254);
    text_arc(c, &elc_bc_34, "AVERAGE MPG", cx, cy, g.L(214), 0, true, kInk, g.L(3));
    brushed_cap(c, cx, cy, g.L(104), g.L(10));

    // Instant sub-gauge below the cap.
    const float px = g.X(400), py = g.Y(590);
    arc_band(c, px, py, g.L(80), g.L(83), psi_inst(0), psi_inst(40), kTick, false);
    for (int v = 0; v <= 40; v += 10) {
      tick(c, px, py, psi_inst((float)v), g.L(70), g.L(84), g.L(v % 20 == 0 ? 3.5f : 2.5f), kTick);
    }
    text(c, &elc_b_22, "0", polar_x(px, g.L(100), psi_inst(0) - 8), polar_y(py, g.L(100), psi_inst(0) - 8),
         Align::Center, kInkDim);
    text(c, &elc_b_22, "40", polar_x(px, g.L(100), psi_inst(40) + 8), polar_y(py, g.L(100), psi_inst(40) + 8),
         Align::Center, kInkDim);
    text(c, &elc_b_22, "INSTANT", px, g.Y(624), Align::Center, kInkDim);

    text_arc(c, &elc_bc_34, "MILES TO EMPTY", cx, cy, g.L(290), 180, false, kInkDim, g.L(3));
    window(c, g.X(328), g.Y(710), g.L(144), g.L(54), g.L(9));
  }

  void build(lv_obj_t *root, const Geo &g) override {
    dte_ = label(root, &elc_bc_46, kInk, g.X(400), g.Y(737), g.L(136), Align::Center);
    inst_ = classic_needle(this, add<Needle>(root), g, g.X(400), g.Y(590), 78, 14, 8, 3);
    hub_at(add<Hub>(root), g.X(400), g.Y(590), g.L(11));
    avg_ = classic_needle(this, add<Needle>(root), g, g.cx(), g.cy(), 296, 40, 15, 5);
    hub_at(add<Hub>(root), g.cx(), g.cy(), g.L(26));
  }

  void update(const GaugeData &d, float dt) override {
    const bool ok = d.fresh && d.econ_valid;
    avg_->set(psi_avg(avg_s_.step(ok ? d.mpg_avg : 0.0f, dt, 3.0f)));
    inst_->set(psi_inst(inst_s_.step(ok ? d.mpg_inst : 0.0f, dt, 5.0f)));
    char b[16];
    if (ok) {
      snprintf(b, sizeof(b), "%d", (int)(d.dte_mi + 0.5f));
    } else {
      snprintf(b, sizeof(b), "---");
    }
    set_text(dte_, b);
  }

 private:
  Needle *avg_ = nullptr, *inst_ = nullptr;
  lv_obj_t *dte_ = nullptr;
  Smooth avg_s_, inst_s_;
};

// ---------------------------------------------------------------- media

class ClassicMedia : public Face {
 public:
  bool uses_sensors() const override { return false; }

  void paint(Canvas888 &c, const Geo &g) override {
    const float cx = g.cx(), cy = g.cy();
    dial_bg(c, g);
    arc_band(c, cx, cy, g.L(352), g.L(362), -140, 140, hex(0x2a2a2c), true);
    for (int k = 0; k <= 8; k++) {
      tick(c, cx, cy, -140.0f + 280.0f * (float)k / 8.0f, g.L(370), g.L(380), g.L(2.5f), kTickMinor);
    }
    chrome_ring(c, g, 318, 330);
    text(c, &elc_icons_56, IC_MUSIC, g.X(400), g.Y(226), Align::Center, kAmber);
    text(c, &elc_bc_34, "NOW PLAYING", g.X(400), g.Y(288), Align::Center, kInkDim);
  }

  void build(lv_obj_t *root, const Geo &g) override {
    g_ = g;
    art_.build(root, g.X(400), g.Y(300), g.L(270), g.L(20), hex(0xd9dde2), g.L(3));
    title_ = label(root, &elc_bc_58, kInk, g.X(400), g.Y(368), g.L(540), Align::Center);
    artist_ = label(root, &elc_b_28, kInkDim, g.X(400), g.Y(430), g.L(500), Align::Center);
    time_ = label(root, &elc_b_22, kInkDim, g.X(400), g.Y(520), g.L(300), Align::Center);
    prog_ = add<ArcFill>(root);
    prog_->cx = g.cx();
    prog_->cy = g.cy();
    prog_->r = g.L(357);
    prog_->width = g.L(10);
    prog_->psi0 = -140;
    prog_->psi1 = 140;
    prog_->c0 = mul(kAmber, 0.55f);
    prog_->c1 = kAmber;
    prog_->tip = true;
  }

  void update(const GaugeData &d, float) override {
    if (art_.update(d.media_connected ? d.art_px : nullptr, d.art_w, d.art_h, d.art_stride, d.art_id)) {
      const bool a = art_.visible();  // with art, the text moves below the disc
      move_label(title_, g_.Y(a ? 505 : 368));
      move_label(artist_, g_.Y(a ? 558 : 430));
      move_label(time_, g_.Y(a ? 612 : 520));
    }
    set_text(title_, media_title(d));
    set_text(artist_, media_artist(d));
    const bool t = d.media_connected && d.media_duration > 0;
    prog_->set(t ? d.media_elapsed / d.media_duration : radio_signal(d));
    char b[64] = "", e[12], u[12];
    if (t) {
      fmt_mmss(e, sizeof(e), d.media_elapsed);
      fmt_mmss(u, sizeof(u), d.media_duration);
      snprintf(b, sizeof(b), "%s  /  %s", e, u);
    } else if (d.media_connected && d.media_detail) {  // the radio: preset, stereo, signal
      snprintf(b, sizeof(b), "%s", d.media_detail);
    }
    set_text(time_, b);
  }

 private:
  Geo g_;
  ArtDisc art_;
  lv_obj_t *title_ = nullptr, *artist_ = nullptr, *time_ = nullptr;
  ArcFill *prog_ = nullptr;
};

// ---------------------------------------------------------------- right display: four gauges

class ClassicRightQuad : public Face {
 public:
  static float psi_fuel(float p) { return -28.0f + 56.0f * clampf(p / 100.0f, 0, 1); }
  static float psi_oil(float psi) { return 70.0f + 40.0f * clampf(psi / 60.0f, 0, 1); }
  static float psi_temp(float f) { return 160.0f + 40.0f * (1.0f - clampf((f - 40.0f) / 210.0f, 0, 1)); }
  static float psi_amp(float a) { return -112.0f + 42.0f * clampf((a + 100.0f) / 200.0f, 0, 1); }

  static void scale(Canvas888 &c, const Geo &g, float p0, float p1) {
    const float cx = g.cx(), cy = g.cy();
    arc_band(c, cx, cy, g.L(298), g.L(302), p0, p1, kTick, false);
    for (int k = 0; k <= 4; k++) {
      const bool major = k % 2 == 0;
      tick(c, cx, cy, p0 + (p1 - p0) * (float)k / 4.0f, g.L(major ? 282 : 290), g.L(major ? 314 : 306),
           g.L(major ? 5 : 3), kTick);
    }
  }

  static void chrome_spoke(Canvas888 &c, const Geo &g, float psi) {
    const float cx = g.cx(), cy = g.cy();
    const float x0 = polar_x(cx, g.L(170), psi), y0 = polar_y(cy, g.L(170), psi);
    const float x1 = polar_x(cx, g.L(420), psi), y1 = polar_y(cy, g.L(420), psi);
    const float len = vlen(x1 - x0, y1 - y0), hw = g.L(17);
    const float ux = (x1 - x0) / len, uy = (y1 - y0) / len;
    const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    // shadow
    fill(c, box_grow(box_seg(x0, y0, x1, y1, hw), g.L(12)), [&](float x, float y, Rgb &o) {
      o = Rgb{0, 0, 0};
      return 0.6f * (1.0f - smooth(-2.0f, g.L(10), sd_obox(x, y, mx + g.L(3), my + g.L(5), ux, uy, len * 0.5f, hw)));
    });
    fill(c, box_grow(box_seg(x0, y0, x1, y1, hw), 2), [&](float x, float y, Rgb &o) {
      const float d = sd_obox(x, y, mx, my, ux, uy, len * 0.5f, hw);
      const float a = cover(d);
      if (a > 0) {
        const float t = clampf((-(x - mx) * uy + (y - my) * ux) / hw, -1.0f, 1.0f);  // across, -1..1
        const float k = fmaxf(0.0f, cosf(t * 1.45f + 0.35f));
        const float e = 1.0f - fabsf(t), e2 = e * e, e4 = e2 * e2, e12 = e4 * e4 * e4;
        const float v = 0.22f + 0.72f * k * sqrtf(k) + 0.2f * e12;
        o = mix(hex(0x2b2e32), hex(0xffffff), clampf(v, 0, 1));
      }
      return a;
    });
  }

  void paint(Canvas888 &c, const Geo &g) override {
    const float cx = g.cx(), cy = g.cy();
    dial_bg(c, g);
    // warning zones
    arc_band(c, cx, cy, g.L(294), g.L(306), psi_fuel(0), psi_fuel(12), kRed, false);
    arc_band(c, cx, cy, g.L(294), g.L(306), psi_oil(0), psi_oil(8), kRed, false);
    arc_band(c, cx, cy, g.L(294), g.L(306), psi_temp(250), psi_temp(230), kRed, false);
    scale(c, g, psi_fuel(0), psi_fuel(100));
    scale(c, g, psi_oil(0), psi_oil(60));
    scale(c, g, psi_temp(250), psi_temp(40));
    scale(c, g, psi_amp(-100), psi_amp(100));
    numeral(c, g, &elc_bc_46, "E", 346, psi_fuel(0) - 3, kInk);
    numeral(c, g, &elc_bc_46, "F", 346, psi_fuel(100) + 3, kInk);
    numeral(c, g, &elc_bc_46, "0", 346, psi_oil(0) - 3, kInk);
    numeral(c, g, &elc_bc_46, "60", 350, psi_oil(60) + 4, kInk);
    numeral(c, g, &elc_bc_46, "H", 346, psi_temp(250) - 3, hex(0xff6a5c));
    numeral(c, g, &elc_bc_46, "C", 346, psi_temp(40) + 3, kInk);
    numeral(c, g, &elc_bc_46, "D", 346, psi_amp(-100) - 3, kInk);
    numeral(c, g, &elc_bc_46, "C", 346, psi_amp(100) + 3, kInk);
    numeral(c, g, &elc_bc_34, "FUEL", 244, 0, kInkDim);
    numeral(c, g, &elc_bc_34, "OIL", 244, 90, kInkDim);
    numeral(c, g, &elc_bc_34, "TEMP", 244, 180, kInkDim);
    numeral(c, g, &elc_bc_34, "AMP", 244, -90, kInkDim);
    for (int k = 0; k < 4; k++) {
      chrome_spoke(c, g, 45.0f + 90.0f * (float)k);
    }
    disc(c, cx, cy, g.L(198), hex(0x111214));
    chrome_ring(c, g, 190, 200);
    brushed_cap(c, cx, cy, g.L(162), g.L(12));
    ball(c, cx, cy, g.L(18));
  }

  void build(lv_obj_t *root, const Geo &g) override {
    for (auto &p : ptr_) {
      p = add<Needle>(root);
      p->cx = g.cx();
      p->cy = g.cy();
      p->style = NeedleStyle::Pointer;
      p->r_start = g.L(262);
      p->len = g.L(334);
      p->w_base = g.L(10);
      p->w_tip = g.L(5);
      p->color = kNeedle;
    }
  }

  void update(const GaugeData &d, float dt) override {
    const bool f = d.fresh;
    ptr_[0]->set(psi_fuel(s_[0].step(f ? d.fuel_pct : 0, dt, 2.0f)));
    ptr_[1]->set(psi_oil(s_[1].step(f ? d.oil_psi : 0, dt, 5.0f)));
    ptr_[2]->set(psi_temp(s_[2].step(f ? d.coolant_f : 40, dt, 2.0f)));
    ptr_[3]->set(psi_amp(s_[3].step(f ? d.amp : 0, dt, 5.0f)));
  }

 private:
  Needle *ptr_[4] = {};
  Smooth s_[4];
};

// ---------------------------------------------------------------- right display: fuel + warning lamps

class ClassicRightFuel : public Face {
 public:
  static float psi_fuel(float p) { return -35.0f + 70.0f * clampf(p / 100.0f, 0, 1); }

  struct Win {
    float p0, p1;
    Rgb off, on;
  };
  static constexpr float kR0 = 212, kR1 = 282;

  void paint(Canvas888 &c, const Geo &g) override {
    const float cx = g.cx(), cy = g.cy();
    dial_bg(c, g);
    ring(c, cx, cy, g.L(300), g.L(306), kTick);
    numeral(c, g, &elc_bc_46, "FUEL", 348, 0, kInk);
    numeral(c, g, &elc_bc_46, "E", 348, -40, kInk);
    numeral(c, g, &elc_bc_46, "F", 348, 40, kInk);
    numeral(c, g, &elc_bc_46, "GEN", 352, -90, kInk);
    numeral(c, g, &elc_bc_46, "OIL", 352, 90, kInk);
    numeral(c, g, &elc_bc_46, "TEMP", 350, 180, kInk);
    for (int k = 0; k <= 8; k++) {
      const bool major = k % 2 == 0;
      tick(c, cx, cy, -35.0f + 70.0f * (float)k / 8.0f, g.L(major ? 248 : 264), g.L(292), g.L(major ? 5.5f : 3),
           k == 0 ? kRed : (major ? kTick : kTickMinor));
    }
    for (const Win &w : wins()) {
      arc_band(c, cx, cy, g.L(kR0 - 5), g.L(kR1 + 5), w.p0 - 2.2f, w.p1 + 2.2f, hex(0xc5ccc3), false);
    }
    brushed_cap(c, cx, cy, g.L(196), g.L(14));
    ball(c, cx, cy, g.L(20));
  }

  void build(lv_obj_t *root, const Geo &g) override {
    int i = 0;
    for (const Win &w : wins()) {
      Lamp *l = add<Lamp>(root);
      l->cx = g.cx();
      l->cy = g.cy();
      l->r0 = g.L(kR0);
      l->r1 = g.L(kR1);
      l->psi0 = w.p0;
      l->psi1 = w.p1;
      l->off = w.off;
      l->on = w.on;
      lamp_[i++] = l;
    }
    ptr_ = add<Needle>(root);
    ptr_->cx = g.cx();
    ptr_->cy = g.cy();
    ptr_->style = NeedleStyle::Pointer;
    ptr_->r_start = g.L(204);
    ptr_->len = g.L(292);
    ptr_->w_base = g.L(11);
    ptr_->w_tip = g.L(5);
    ptr_->color = kNeedle;
  }

  void update(const GaugeData &d, float dt) override {
    ptr_->set(psi_fuel(fuel_.step(d.fresh ? d.fuel_pct : 0, dt, 2.0f)));
    const bool running = d.fresh && d.rpm > 400.0f;
    lamp_[0]->set(d.fresh && d.charging == 0);
    lamp_[1]->set(running && d.oil_psi < 8.0f);
    lamp_[2]->set(d.fresh && d.coolant_f > 230.0f);
  }

 private:
  static const Win *wins_arr() {
    static const Win w[3] = {
        {-116, -64, hex(0x2c1c0a), hex(0xffa53a)},  // GEN
        {64, 116, hex(0x2c0d0b), hex(0xff3b2f)},    // OIL
        {154, 206, hex(0x2c0d0b), hex(0xff3b2f)},   // TEMP
    };
    return w;
  }
  struct Wins {
    const Win *begin() const { return wins_arr(); }
    const Win *end() const { return wins_arr() + 3; }
  };
  static Wins wins() { return Wins{}; }

  Needle *ptr_ = nullptr;
  Lamp *lamp_[3] = {};
  Smooth fuel_;
};

}  // namespace

Face *make_classic(GaugeFace f, uint8_t variant) {
  switch (f) {
    case GaugeFace::Speedo:
      return new ClassicSpeedo();
    case GaugeFace::Tach:
      return new ClassicTach();
    case GaugeFace::Clock:
      return new ClassicClock();
    case GaugeFace::Econ:
      return new ClassicEcon();
    case GaugeFace::Media:
      return new ClassicMedia();
    case GaugeFace::Right:
    default:
      return variant == 1 ? (Face *)new ClassicRightFuel() : (Face *)new ClassicRightQuad();
  }
}

}  // namespace gauge
