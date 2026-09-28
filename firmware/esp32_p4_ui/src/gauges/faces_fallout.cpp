// Fallout theme: green phosphor terminal. VT323 text, segment bars and CRT scanlines over everything.
// The painter runs per pixel: always build it optimised, even in -Os firmware builds.
#pragma GCC optimize("O3", "fast-math")

#include <stdio.h>

#include "face.h"

namespace gauge {

namespace {

const Rgb kBg = hex(0x010a04);
const Rgb kFg = hex(0x5dff8f), kDim = hex(0x2fae5c), kFaint = hex(0x17602f), kHot = hex(0xb8ffcc);

void bg(Canvas888 &c, const Geo &g) {
  const float cx = g.cx(), cy = g.cy();
  fill(c, Box{0, 0, (float)c.w, (float)c.h}, [&](float x, float y, Rgb &o) {
    const float d = clampf((vlen(x - cx, y - cy) - g.L(150)) / g.L(262), 0, 1);
    o = mix(mix(kBg, hex(0x0a3a1a), 0.35f * (1.0f - d)), hex(0x000000), 0.55f * d * d);
    return 1.0f;
  });
}

void header(Canvas888 &c, const Geo &g, const char *s) {
  text(c, &elc_vt_56, s, g.X(400), g.Y(134), Align::Center, kFg);
}

/** Row of block segments; only repaints when the number of lit segments changes. */
class SegBar {
 public:
  void move_y(const Geo &g, float y) {
    for (int i = 0; i < n_; i++) {
      lv_obj_set_y(seg_[i], (lv_coord_t)lroundf(g.Y(y)));
    }
  }
  void build(lv_obj_t *root, const Geo &g, float x, float y, int n, float w, float h, float gap) {
    n_ = n < kMax ? n : kMax;
    for (int i = 0; i < n_; i++) {
      seg_[i] = rect(root, g.X(x + (w + gap) * (float)i), g.Y(y), g.L(w), g.L(h), kFaint);
    }
  }
  void set(float frac) {
    const int lit = (int)lroundf(clampf(frac, 0, 1) * (float)n_);
    if (lit == lit_) {
      return;
    }
    lit_ = lit;
    for (int i = 0; i < n_; i++) {
      set_bg(seg_[i], i < lit ? kFg : kFaint);
    }
  }

 private:
  static constexpr int kMax = 16;
  lv_obj_t *seg_[kMax] = {};
  int n_ = 0, lit_ = -1;
};

/** Faces add their parts, then the scanlines last so they cover the text too. */
class FalloutFace : public Face {
 protected:
  void scanlines(lv_obj_t *root, const Geo &g) {
    Scanlines *s = add<Scanlines>(root);
    s->period = fmaxf(3.0f, roundf(g.L(5)));
  }
};

const char *kDash = "-";

// ---------------------------------------------------------------- speedometer

class FalloutSpeedo : public FalloutFace {
 public:
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    header(c, g, "- STAT -");
    text(c, &elc_vt_56, "MPH", g.X(400), g.Y(452), Align::Center, kDim);
    text(c, &elc_vt_56, "FUEL", g.X(187), g.Y(545), Align::Left, kFg);
  }
  void build(lv_obj_t *root, const Geo &g) override {
    speed_ = label(root, &elc_vt_220d, kFg, g.X(400), g.Y(330), g.L(520), Align::Center);
    fuel_.build(root, g, 305, 533, 10, 25, 22, 6);
    odo_ = label(root, &elc_vt_56, kDim, g.X(400), g.Y(630), g.L(520), Align::Center);
    scanlines(root, g);
  }
  void update(const GaugeData &d, float) override {
    char b[40];
    snprintf(b, sizeof(b), "%03d", d.fresh ? (int)(d.mph + 0.5f) : 0);
    set_text(speed_, b);
    fuel_.set(d.fresh ? d.fuel_pct / 100.0f : 0);
    if (d.banner && d.banner[0]) {
      snprintf(b, sizeof(b), "%s", d.banner);
    } else if (d.odo_valid) {
      snprintf(b, sizeof(b), "ODO %.1f", (double)d.odo_mi);
    } else {
      snprintf(b, sizeof(b), "ODO -----.-");
    }
    set_text(odo_, b);
  }

 private:
  lv_obj_t *speed_ = nullptr, *odo_ = nullptr;
  SegBar fuel_;
};

// ---------------------------------------------------------------- center: speed / rpm / trip

class FalloutTach : public FalloutFace {
 public:
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    header(c, g, "- SPEED -");
    text(c, &elc_vt_36, "MPH", g.X(590), g.Y(272), Align::Left, kDim);
    const char *rows[4] = {"RPM", "AP", "TRIP", "ODO"};
    for (int i = 0; i < 4; i++) {
      text(c, &elc_vt_56, rows[i], g.X(187), g.Y(360 + 70 * (float)i), Align::Left, kDim);
    }
  }
  void build(lv_obj_t *root, const Geo &g) override {
    for (int i = 0; i < 3; i++) {
      gear_box_[i] = rect(root, g.X(200 + 55 * (float)i), g.Y(222), g.L(40), g.L(54), kFg);
      gear_l_[i] = label(root, &elc_vt_56, kDim, g.X(220 + 55 * (float)i), g.Y(249), g.L(40), Align::Center);
      char b[4];
      snprintf(b, sizeof(b), "%d", i + 1);
      lv_label_set_text(gear_l_[i], b);
    }
    speed_ = label(root, &elc_vt_120d, kFg, g.X(582), g.Y(246), g.L(220), Align::Right);
    rpm_ = label(root, &elc_vt_56, kFg, g.X(612), g.Y(360), g.L(300), Align::Right);
    amp_.build(root, g, 318, 419, 9, 25, 22, 6);
    trip_ = label(root, &elc_vt_56, kFg, g.X(612), g.Y(500), g.L(300), Align::Right);
    odo_ = label(root, &elc_vt_56, kFg, g.X(612), g.Y(570), g.L(300), Align::Right);
    scanlines(root, g);
  }
  void update(const GaugeData &d, float) override {
    const int gear = d.fresh ? d.gear : 0;
    if (gear != gear_) {
      gear_ = gear;
      for (int i = 0; i < 3; i++) {
        show(gear_box_[i], gear == i + 1);
        set_color(gear_l_[i], gear == i + 1 ? kBg : kDim);
      }
    }
    char b[32];
    snprintf(b, sizeof(b), "%03d", d.fresh ? (int)(d.mph + 0.5f) : 0);
    set_text(speed_, b);
    if (d.fresh) snprintf(b, sizeof(b), "%d", (int)d.rpm);
    else snprintf(b, sizeof(b), "----");
    set_text(rpm_, b);
    amp_.set(d.fresh ? (d.amp + 100.0f) / 200.0f : 0);
    if (d.odo_valid) snprintf(b, sizeof(b), "%.1f MI", (double)d.trip_mi);
    else snprintf(b, sizeof(b), "--- MI");
    set_text(trip_, b);
    if (d.odo_valid) snprintf(b, sizeof(b), "%d MI", (int)d.odo_mi);
    else snprintf(b, sizeof(b), "----- MI");
    set_text(odo_, b);
  }

 private:
  lv_obj_t *gear_box_[3] = {}, *gear_l_[3] = {};
  int gear_ = -1;
  lv_obj_t *speed_ = nullptr, *rpm_ = nullptr, *trip_ = nullptr, *odo_ = nullptr;
  SegBar amp_;
};

// ---------------------------------------------------------------- clock

class FalloutClock : public FalloutFace {
 public:
  bool uses_sensors() const override { return false; }
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    header(c, g, "- TIME -");
  }
  void build(lv_obj_t *root, const Geo &g) override {
    time_ = label(root, &elc_vt_220d, kFg, g.X(400), g.Y(350), g.L(640), Align::Center);
    ampm_ = label(root, &elc_vt_56, kDim, g.X(400), g.Y(470), g.L(200), Align::Center);
    date_ = label(root, &elc_vt_56, kFg, g.X(400), g.Y(560), g.L(560), Align::Center);
    place_ = label(root, &elc_vt_36, kDim, g.X(400), g.Y(625), g.L(480), Align::Center);
    scanlines(root, g);
  }
  void update(const GaugeData &d, float) override {
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", hour12(d.hour), d.minute);
    set_text(time_, b);
    set_text(ampm_, d.hour < 12 ? "AM" : "PM");
    set_text(date_, d.time_valid ? (d.date_text ? d.date_text : "") : "NO TIME SOURCE");
    set_color(date_, d.time_valid ? kFg : kHot);
    set_text(place_, d.place_text ? d.place_text : "");
  }

 private:
  lv_obj_t *time_ = nullptr, *ampm_ = nullptr, *date_ = nullptr, *place_ = nullptr;
};

// ---------------------------------------------------------------- fuel economy

class FalloutEcon : public FalloutFace {
 public:
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    header(c, g, "- FUEL -");
    text(c, &elc_vt_36, "AVG MPG", g.X(400), g.Y(372), Align::Center, kDim);
    text(c, &elc_vt_56, "INST", g.X(187), g.Y(452), Align::Left, kDim);
    text(c, &elc_vt_56, "RANGE", g.X(187), g.Y(530), Align::Left, kDim);
    text(c, &elc_vt_56, "TANK", g.X(187), g.Y(600), Align::Left, kDim);
  }
  void build(lv_obj_t *root, const Geo &g) override {
    avg_ = label(root, &elc_vt_120d, kFg, g.X(400), g.Y(285), g.L(360), Align::Center);
    inst_.build(root, g, 318, 441, 9, 25, 22, 6);
    range_ = label(root, &elc_vt_56, kFg, g.X(612), g.Y(530), g.L(260), Align::Right);
    tank_ = label(root, &elc_vt_56, kFg, g.X(612), g.Y(600), g.L(260), Align::Right);
    scanlines(root, g);
  }
  void update(const GaugeData &d, float) override {
    const bool ok = d.fresh && d.econ_valid;
    char b[24];
    if (ok) snprintf(b, sizeof(b), "%.1f", (double)d.mpg_avg);
    else snprintf(b, sizeof(b), "--.-");
    set_text(avg_, b);
    inst_.set(ok ? d.mpg_inst / 40.0f : 0);
    if (ok) snprintf(b, sizeof(b), "%d MI", (int)(d.dte_mi + 0.5f));
    else snprintf(b, sizeof(b), "--- MI");
    set_text(range_, b);
    if (d.fresh) snprintf(b, sizeof(b), "%.1f GAL", (double)(d.fuel_pct / 100.0f * d.tank_gal));
    else snprintf(b, sizeof(b), "-- GAL");
    set_text(tank_, b);
  }

 private:
  lv_obj_t *avg_ = nullptr, *range_ = nullptr, *tank_ = nullptr;
  SegBar inst_;
};

// ---------------------------------------------------------------- media

class FalloutMedia : public FalloutFace {
 public:
  bool uses_sensors() const override { return false; }
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    header(c, g, "- RADIO -");
  }
  void build(lv_obj_t *root, const Geo &g) override {
    g_ = g;
    art_.build(root, g.X(400), g.Y(290), g.L(230), kDim, g.L(3), true, kBg, kFg);
    title_ = label(root, &elc_vt_72, kFg, g.X(400), g.Y(320), g.L(560), Align::Center);
    artist_ = label(root, &elc_vt_56, kDim, g.X(400), g.Y(395), g.L(540), Align::Center);
    prog_.build(root, g, 212, 470, 12, 25, 22, 6.3f);
    time_ = label(root, &elc_vt_56, kFg, g.X(400), g.Y(565), g.L(400), Align::Center);
    scanlines(root, g);
  }
  void update(const GaugeData &d, float) override {
    if (art_.update(d.media_connected ? d.art_px : nullptr, d.art_w, d.art_h, d.art_stride, d.art_id)) {
      const bool a = art_.visible();
      move_label(title_, g_.Y(a ? 462 : 320));
      move_label(artist_, g_.Y(a ? 522 : 395));
      prog_.move_y(g_, a ? 560 : 470);
      move_label(time_, g_.Y(a ? 632 : 565));
    }
    set_text(title_, media_title(d));
    set_text(artist_, media_artist(d));
    const bool t = d.media_connected && d.media_duration > 0;
    prog_.set(t ? d.media_elapsed / d.media_duration : 0);
    char b[32] = "", e[12], u[12];
    if (t) {
      fmt_mmss(e, sizeof(e), d.media_elapsed);
      fmt_mmss(u, sizeof(u), d.media_duration);
      snprintf(b, sizeof(b), "%s / %s", e, u);
    }
    set_text(time_, b);
  }

 private:
  Geo g_;
  ArtDisc art_;
  lv_obj_t *title_ = nullptr, *artist_ = nullptr, *time_ = nullptr;
  SegBar prog_;
};

// ---------------------------------------------------------------- right display

class FalloutRight : public FalloutFace {
 public:
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    header(c, g, "- HP -");
    const char *rows[4] = {"FUEL", "TEMP", "OIL", "AMP"};
    for (int i = 0; i < 4; i++) {
      text(c, &elc_vt_56, rows[i], g.X(220), g.Y(383 + 62.5f * (float)i), Align::Left, kDim);
    }
  }
  void build(lv_obj_t *root, const Geo &g) override {
    hp_bar_.build(root, g, 212, 210, 10, 30, 22, 7.5f);
    hp_ = label(root, &elc_vt_56, kFg, g.X(400), g.Y(292), g.L(200), Align::Center);
    for (int i = 0; i < 4; i++) {
      val_[i] = label(root, &elc_vt_56, kFg, g.X(580), g.Y(383 + 62.5f * (float)i), g.L(240), Align::Right);
    }
    scanlines(root, g);
  }
  void update(const GaugeData &d, float) override {
    const bool f = d.fresh;
    const bool low_oil = f && d.rpm > 400 && d.oil_psi < 8;
    const float hp = f ? clampf(100.0f - (low_oil ? 60.0f : 0.0f) - fmaxf(0.0f, d.coolant_f - 215.0f) * 2.0f, 0, 100) : 0;
    hp_bar_.set(hp / 100.0f);
    char b[24];
    snprintf(b, sizeof(b), "%d%%", (int)hp);
    set_text(hp_, b);
    if (f) {
      snprintf(b, sizeof(b), "%d%%", (int)d.fuel_pct);
      set_text(val_[0], b);
      snprintf(b, sizeof(b), "%d F", (int)d.coolant_f);
      set_text(val_[1], b);
      snprintf(b, sizeof(b), "%d PSI", (int)(d.oil_psi + 0.5f));
      set_text(val_[2], b);
      snprintf(b, sizeof(b), "%+d", (int)d.amp);
      set_text(val_[3], b);
    } else {
      for (auto *v : val_) {
        set_text(v, kDash);
      }
    }
  }

 private:
  SegBar hp_bar_;
  lv_obj_t *hp_ = nullptr, *val_[4] = {};
};

}  // namespace

Face *make_fallout(GaugeFace f) {
  switch (f) {
    case GaugeFace::Speedo:
      return new FalloutSpeedo();
    case GaugeFace::Tach:
      return new FalloutTach();
    case GaugeFace::Clock:
      return new FalloutClock();
    case GaugeFace::Econ:
      return new FalloutEcon();
    case GaugeFace::Media:
      return new FalloutMedia();
    case GaugeFace::Right:
    default:
      return new FalloutRight();
  }
}

}  // namespace gauge
