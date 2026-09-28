// Modern theme: flat dark glass, thin ticks, one orange accent, big Saira numerals. Ported from the
// cluster prototype (its 640-unit canvas scales ×1.25 to the 800-unit face).
// The painter runs per pixel: always build it optimised, even in -Os firmware builds.
#pragma GCC optimize("O3", "fast-math")

#include <stdio.h>

#include "face.h"

namespace gauge {

namespace {

const Rgb kBg0 = hex(0x15181b), kBg1 = hex(0x060708);
const Rgb kTrack = hex(0x23282c);
const Rgb kTickLo = hex(0x5d666c), kTickHi = hex(0xc9d0d4);
const Rgb kText = hex(0xf3f5f6), kDim = hex(0x9aa4aa), kFaint = hex(0x5d666c);
const Rgb kAccent = hex(0xff7a2f), kAccent2 = hex(0xffb07a);
const Rgb kRed = hex(0xff3b30);

#define IC_MUSIC "\xEE\x90\x85"

void bg(Canvas888 &c, const Geo &g) {
  const float cx = g.X(400), cy = g.Y(350), R = g.L(412);
  fill(c, Box{0, 0, (float)c.w, (float)c.h}, [&](float x, float y, Rgb &o) {
    const float d = clampf((vlen(x - cx, y - cy) - g.L(50)) / (R - g.L(50)), 0, 1);
    o = mix(kBg0, kBg1, d);
    return 1.0f;
  });
}

void ticks(Canvas888 &c, const Geo &g, float p0, float p1, int n, int major_every, const char *const *labels,
           float red_from = 2.0f) {
  const float cx = g.cx(), cy = g.cy();
  for (int i = 0; i <= n; i++) {
    const float psi = p0 + (p1 - p0) * (float)i / (float)n;
    const bool major = i % major_every == 0;
    const bool red = (float)i / (float)n >= red_from;
    tick(c, cx, cy, psi, g.L(major ? 337.5f : 349.9f), g.L(365), g.L(major ? 3.75f : 2.0f),
         red ? kRed : (major ? kTickHi : kTickLo), true);
    if (major && labels) {
      text(c, &elc_sm_28, labels[i / major_every], polar_x(cx, g.L(270), psi), polar_y(cy, g.L(270), psi),
           Align::Center, red ? kRed : kDim);
    }
  }
}

ArcFill *progress(Face *, ArcFill *a, const Geo &g, float r, float w, float p0, float p1) {
  a->cx = g.cx();
  a->cy = g.cy();
  a->r = g.L(r);
  a->width = g.L(w);
  a->psi0 = p0;
  a->psi1 = p1;
  a->c0 = mix(kTrack, kAccent, 0.35f);
  a->c1 = kAccent;
  a->tip = true;
  return a;
}

void track(Canvas888 &c, const Geo &g, float r, float w, float p0, float p1, bool round = true) {
  arc_band(c, g.cx(), g.cy(), g.L(r - w * 0.5f), g.L(r + w * 0.5f), p0, p1, kTrack, round);
}

// ---------------------------------------------------------------- speedometer

class ModernSpeedo : public Face {
 public:
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    static const char *const kL[] = {"0", "20", "40", "60", "80", "100", "120"};
    ticks(c, g, -130, 130, 24, 4, kL);
    track(c, g, 312.5f, 17.5f, -130, 130);
    text(c, &elc_sm_28, "MPH", g.X(400), g.Y(500), Align::Center, kDim);
    text(c, &elc_sm_16, "ODOMETER", g.X(400), g.Y(684), Align::Center, kFaint);
  }
  void build(lv_obj_t *root, const Geo &g) override {
    arc_ = progress(this, add<ArcFill>(root), g, 312.5f, 17.5f, -130, 130);
    speed_ = label(root, &elc_sa_180d, kText, g.X(400), g.Y(398), g.L(520), Align::Center);
    odo_ = label(root, &elc_sm_28, kDim, g.X(400), g.Y(648), g.L(420), Align::Center);
  }
  void update(const GaugeData &d, float dt) override {
    const float v = mph_.step(d.fresh ? d.mph : 0, dt, 8.0f);
    arc_->set(v / 120.0f);
    char b[32];
    snprintf(b, sizeof(b), "%d", (int)(d.fresh ? d.mph + 0.5f : 0));
    set_text(speed_, b);
    if (d.banner && d.banner[0]) {
      snprintf(b, sizeof(b), "%s", d.banner);
      set_color(odo_, kAccent);
    } else {
      if (d.odo_valid) {
        snprintf(b, sizeof(b), "%.1f mi", (double)d.odo_mi);
      } else {
        snprintf(b, sizeof(b), "\xE2\x80\x94 mi");
      }
      set_color(odo_, kDim);
    }
    set_text(odo_, b);
  }

 private:
  ArcFill *arc_ = nullptr;
  lv_obj_t *speed_ = nullptr, *odo_ = nullptr;
  Smooth mph_;
};

// ---------------------------------------------------------------- tachometer

class ModernTach : public Face {
 public:
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    static const char *const kL[] = {"0", "1", "2", "3", "4", "5", "6"};
    ticks(c, g, -130, 130, 12, 2, kL, 5.0f / 6.0f);
    track(c, g, 312.5f, 17.5f, -130, 130);
    arc_band(c, g.cx(), g.cy(), g.L(303.75f), g.L(321.25f), -130 + 260 * 5.0f / 6.0f, 130, mix(kTrack, kRed, 0.35f), true);
    text(c, &elc_sm_28, "RPM", g.X(400), g.Y(474), Align::Center, kDim);
    round_rect_ring(c, g.X(355), g.Y(212), g.L(90), g.L(70), g.L(17.5f), g.L(2.5f), kTrack);
    text(c, &elc_sm_16, "COOLANT", g.X(312), g.Y(606), Align::Center, kFaint);
    text(c, &elc_sm_16, "CHARGE", g.X(488), g.Y(606), Align::Center, kFaint);
  }
  void build(lv_obj_t *root, const Geo &g) override {
    arc_ = progress(this, add<ArcFill>(root), g, 312.5f, 17.5f, -130, 130);
    rpm_ = label(root, &elc_sa_136d, kText, g.X(400), g.Y(390), g.L(460), Align::Center);
    gear_ = label(root, &elc_sa_46, kAccent, g.X(400), g.Y(248), g.L(86), Align::Center);
    temp_ = label(root, &elc_sa_38, kText, g.X(312), g.Y(572), g.L(170), Align::Center);
    chg_ = label(root, &elc_sa_38, kText, g.X(488), g.Y(572), g.L(170), Align::Center);
    song_ = label(root, &elc_sm_22, kDim, g.X(400), g.Y(684), g.L(340), Align::Center);
  }
  void update(const GaugeData &d, float dt) override {
    const float v = rpm_s_.step(d.fresh ? d.rpm : 0, dt, 10.0f);
    arc_->set(v / 6000.0f);
    arc_->set_override(v > 5000.0f, kRed);
    char b[48];
    const int r = d.fresh ? (int)d.rpm : 0;
    if (r >= 1000) {
      snprintf(b, sizeof(b), "%d,%03d", r / 1000, r % 1000);
    } else {
      snprintf(b, sizeof(b), "%d", r);
    }
    set_text(rpm_, b);
    if (d.fresh && d.gear > 0) {
      snprintf(b, sizeof(b), "D%d", d.gear);
    } else {
      snprintf(b, sizeof(b), "P");
    }
    set_text(gear_, b);
    if (d.fresh) {
      snprintf(b, sizeof(b), "%d\xC2\xB0", (int)d.coolant_f);
    } else {
      snprintf(b, sizeof(b), "\xE2\x80\x94");
    }
    set_text(temp_, b);
    set_color(temp_, d.fresh && d.coolant_f > 230 ? kRed : kText);
    set_text(chg_, !d.fresh ? "\xE2\x80\x94" : d.charging == 1 ? "CHG" : d.charging == 0 ? "NO CHG" : "OFF");
    set_color(chg_, d.fresh && d.charging == 0 ? kRed : kText);
    b[0] = 0;
    if (d.media_connected && d.media_title && d.media_title[0]) {
      snprintf(b, sizeof(b), "%s", d.media_title);
    }
    set_text(song_, b);
  }

 private:
  ArcFill *arc_ = nullptr;
  lv_obj_t *rpm_ = nullptr, *gear_ = nullptr, *temp_ = nullptr, *chg_ = nullptr, *song_ = nullptr;
  Smooth rpm_s_;
};

// ---------------------------------------------------------------- clock

class ModernClock : public Face {
 public:
  bool uses_sensors() const override { return false; }
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    ring(c, g.cx(), g.cy(), g.L(355), g.L(360), kTrack);
  }
  void build(lv_obj_t *root, const Geo &g) override {
    sec_ = add<ArcFill>(root);
    sec_->cx = g.cx();
    sec_->cy = g.cy();
    sec_->r = g.L(357.5f);
    sec_->width = g.L(5);
    sec_->psi0 = 0;
    sec_->psi1 = 360;
    sec_->c0 = sec_->c1 = kAccent;
    date_ = label(root, &elc_sm_28, kDim, g.X(400), g.Y(262), g.L(460), Align::Center);
    time_ = label(root, &elc_sa_180d, kText, g.X(400), g.Y(398), g.L(600), Align::Center);
    ampm_ = label(root, &elc_sm_28, kDim, g.X(400), g.Y(494), g.L(120), Align::Center);
    place_ = label(root, &elc_sm_28, kFaint, g.X(400), g.Y(582), g.L(460), Align::Center);
  }
  void update(const GaugeData &d, float) override {
    sec_->set(d.second / 60.0f);
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", hour12(d.hour), d.minute);
    set_text(time_, b);
    set_text(ampm_, d.hour < 12 ? "AM" : "PM");
    set_text(date_, d.time_valid ? (d.date_text ? d.date_text : "") : "No time source");
    set_color(date_, d.time_valid ? kDim : kRed);
    set_text(place_, d.place_text ? d.place_text : "");
  }

 private:
  ArcFill *sec_ = nullptr;
  lv_obj_t *date_ = nullptr, *time_ = nullptr, *ampm_ = nullptr, *place_ = nullptr;
};

// ---------------------------------------------------------------- fuel economy

class ModernEcon : public Face {
 public:
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    track(c, g, 327.5f, 15, -130, 130);
    text(c, &elc_sm_22, "TRIP AVG MPG", g.X(400), g.Y(432), Align::Center, kDim);
    round_rect(c, g.X(245), g.Y(500), g.L(310), g.L(12.5f), g.L(6.25f), kTrack);
    text(c, &elc_sm_16, "RANGE", g.X(312), g.Y(650), Align::Center, kFaint);
    text(c, &elc_sm_16, "IN TANK", g.X(488), g.Y(650), Align::Center, kFaint);
  }
  void build(lv_obj_t *root, const Geo &g) override {
    g_ = g;
    arc_ = progress(this, add<ArcFill>(root), g, 327.5f, 15, -130, 130);
    avg_ = label(root, &elc_sa_136d, kText, g.X(400), g.Y(338), g.L(420), Align::Center);
    bar_ = rect(root, g.X(245), g.Y(500), g.L(12.5f), g.L(12.5f), kAccent2, g.L(6.25f));
    inst_ = label(root, &elc_sm_22, kFaint, g.X(400), g.Y(544), g.L(300), Align::Center);
    range_ = label(root, &elc_sa_38, kText, g.X(312), g.Y(614), g.L(170), Align::Center);
    tank_ = label(root, &elc_sa_38, kText, g.X(488), g.Y(614), g.L(170), Align::Center);
  }
  void update(const GaugeData &d, float dt) override {
    const bool ok = d.fresh && d.econ_valid;
    arc_->set(avg_s_.step(ok ? d.mpg_avg : 0, dt, 3.0f) / 40.0f);
    const float inst = inst_s_.step(ok ? d.mpg_inst : 0, dt, 5.0f);
    const lv_coord_t w = (lv_coord_t)fmaxf(g_.L(12.5f), g_.L(310) * clampf(inst / 40.0f, 0, 1));
    if (lv_obj_get_style_width(bar_, 0) != w) {
      lv_obj_set_width(bar_, w);
    }
    char b[32];
    const char *dash = "\xE2\x80\x94";
    if (ok) {
      snprintf(b, sizeof(b), "%.1f", (double)d.mpg_avg);
    } else {
      snprintf(b, sizeof(b), "%s", dash);
    }
    set_text(avg_, b);
    if (ok) {
      snprintf(b, sizeof(b), "INSTANT %d", (int)(d.mpg_inst + 0.5f));
    } else {
      snprintf(b, sizeof(b), "INSTANT %s", dash);
    }
    set_text(inst_, b);
    if (ok) {
      snprintf(b, sizeof(b), "%d mi", (int)(d.dte_mi + 0.5f));
    } else {
      snprintf(b, sizeof(b), "%s mi", dash);
    }
    set_text(range_, b);
    if (d.fresh) {
      snprintf(b, sizeof(b), "%.1f gal", (double)(d.fuel_pct / 100.0f * d.tank_gal));
    } else {
      snprintf(b, sizeof(b), "%s", dash);
    }
    set_text(tank_, b);
  }

 private:
  Geo g_;
  ArcFill *arc_ = nullptr;
  lv_obj_t *avg_ = nullptr, *bar_ = nullptr, *inst_ = nullptr, *range_ = nullptr, *tank_ = nullptr;
  Smooth avg_s_, inst_s_;
};

// ---------------------------------------------------------------- media

class ModernMedia : public Face {
 public:
  bool uses_sensors() const override { return false; }
  void paint(Canvas888 &c, const Geo &g) override {
    bg(c, g);
    ring(c, g.cx(), g.cy(), g.L(347.5f), g.L(357.5f), kTrack);
    text(c, &elc_icons_56, IC_MUSIC, g.X(400), g.Y(262), Align::Center, kDim);
  }
  void build(lv_obj_t *root, const Geo &g) override {
    g_ = g;
    art_.build(root, g.X(400), g.Y(295), g.L(280), g.L(20), kTrack, g.L(2));
    prog_ = progress(this, add<ArcFill>(root), g, 352.5f, 10, 0, 360);
    prog_->c0 = prog_->c1 = kAccent;
    prog_->tip = false;
    title_ = label(root, &elc_sa_56, kText, g.X(400), g.Y(380), g.L(580), Align::Center);
    artist_ = label(root, &elc_sm_28, kDim, g.X(400), g.Y(440), g.L(540), Align::Center);
    time_ = label(root, &elc_sm_22, kFaint, g.X(400), g.Y(520), g.L(300), Align::Center);
  }
  void update(const GaugeData &d, float) override {
    if (art_.update(d.media_connected ? d.art_px : nullptr, d.art_w, d.art_h, d.art_stride, d.art_id)) {
      const bool a = art_.visible();
      move_label(title_, g_.Y(a ? 512 : 380));
      move_label(artist_, g_.Y(a ? 568 : 440));
      move_label(time_, g_.Y(a ? 622 : 520));
    }
    set_text(title_, media_title(d));
    set_text(artist_, media_artist(d));
    const bool t = d.media_connected && d.media_duration > 0;
    prog_->set(t ? d.media_elapsed / d.media_duration : 0.0f);
    char b[32] = "", e[12], u[12];
    if (t) {
      fmt_mmss(e, sizeof(e), d.media_elapsed);
      fmt_mmss(u, sizeof(u), d.media_duration);
      snprintf(b, sizeof(b), "%s  /  %s", e, u);
    }
    set_text(time_, b);
  }

 private:
  Geo g_;
  ArtDisc art_;
  ArcFill *prog_ = nullptr;
  lv_obj_t *title_ = nullptr, *artist_ = nullptr, *time_ = nullptr;
};

// ---------------------------------------------------------------- right display

class ModernRight : public Face {
 public:
  void paint(Canvas888 &c, const Geo &g) override {
    const float cx = g.cx(), cy = g.cy(), r = g.L(340);
    bg(c, g);
    track(c, g, 340, 22.5f, -150, -30, false);
    track(c, g, 340, 22.5f, 30, 150, false);
    text(c, &elc_sm_22, "E", polar_x(cx, r, -157), polar_y(cy, r, -157), Align::Center, kFaint);
    text(c, &elc_sm_22, "F", polar_x(cx, r, -23), polar_y(cy, r, -23), Align::Center, kFaint);
    text(c, &elc_sm_22, "C", polar_x(cx, r, 157), polar_y(cy, r, 157), Align::Center, kFaint);
    text(c, &elc_sm_22, "H", polar_x(cx, r, 23), polar_y(cy, r, 23), Align::Center, kRed);
    text(c, &elc_sm_16, "FUEL", g.X(245), g.Y(418), Align::Center, kFaint);
    text(c, &elc_sm_16, "COOLANT", g.X(555), g.Y(418), Align::Center, kFaint);
    text(c, &elc_sm_22, "OIL PSI", g.X(400), g.Y(410), Align::Center, kDim);
  }
  void build(lv_obj_t *root, const Geo &g) override {
    fuel_ = add<ArcFill>(root);
    temp_ = add<ArcFill>(root);
    ArcFill *both[2] = {fuel_, temp_};
    for (ArcFill *a : both) {
      a->cx = g.cx();
      a->cy = g.cy();
      a->r = g.L(340);
      a->width = g.L(22.5f);
      a->round_caps = false;
    }
    fuel_->psi0 = -150;
    fuel_->psi1 = -30;
    fuel_->c0 = fuel_->c1 = kAccent;
    temp_->psi0 = 30;
    temp_->psi1 = 150;
    temp_->from_end = true;
    temp_->c0 = temp_->c1 = kTickHi;
    fuel_v_ = label(root, &elc_sa_38, kText, g.X(245), g.Y(382), g.L(150), Align::Center);
    temp_v_ = label(root, &elc_sa_38, kText, g.X(555), g.Y(382), g.L(150), Align::Center);
    oil_v_ = label(root, &elc_sa_136d, kText, g.X(400), g.Y(328), g.L(260), Align::Center);
    const char *names[3] = {"GEN", "OIL", "TEMP"};
    for (int i = 0; i < 3; i++) {
      const float x = g.X(300 + 100 * (float)i);
      chip_[i] = rect(root, x - g.L(42), g.Y(531), g.L(84), g.L(37.5f), hex(0xffffff), g.L(18.75f), 0.05f);
      chip_l_[i] = label(root, &elc_sm_16, kFaint, x, g.Y(550), g.L(84), Align::Center);
      lv_label_set_text(chip_l_[i], names[i]);
    }
    chg_ = label(root, &elc_sm_22, kDim, g.X(400), g.Y(628), g.L(300), Align::Center);
  }
  void update(const GaugeData &d, float dt) override {
    const bool f = d.fresh;
    const float fuel = fuel_s_.step(f ? d.fuel_pct : 0, dt, 2.0f);
    const float temp = temp_s_.step(f ? d.coolant_f : 100, dt, 2.0f);
    const bool low_fuel = f && d.fuel_pct < 12;
    const bool hot = f && d.coolant_f > 230;
    const bool low_oil = f && d.rpm > 400 && d.oil_psi < 8;
    const bool gen = f && d.charging == 0;
    fuel_->set(fuel / 100.0f);
    fuel_->set_override(low_fuel, kRed);
    temp_->set(clampf((temp - 100.0f) / 160.0f, 0, 1));
    temp_->set_override(hot, kRed);
    char b[24];
    const char *dash = "\xE2\x80\x94";
    if (f) snprintf(b, sizeof(b), "%d%%", (int)d.fuel_pct);
    else snprintf(b, sizeof(b), "%s", dash);
    set_text(fuel_v_, b);
    set_color(fuel_v_, low_fuel ? kRed : kText);
    if (f) snprintf(b, sizeof(b), "%d\xC2\xB0", (int)d.coolant_f);
    else snprintf(b, sizeof(b), "%s", dash);
    set_text(temp_v_, b);
    set_color(temp_v_, hot ? kRed : kText);
    if (f) snprintf(b, sizeof(b), "%d", (int)(d.oil_psi + 0.5f));
    else snprintf(b, sizeof(b), "%s", dash);
    set_text(oil_v_, b);
    set_color(oil_v_, low_oil ? kRed : kText);
    const bool on[3] = {gen, low_oil, hot};
    for (int i = 0; i < 3; i++) {
      if (on[i] != chip_on_[i]) {
        chip_on_[i] = on[i];
        lv_obj_set_style_bg_color(chip_[i], to_lv(on[i] ? kRed : hex(0xffffff)), 0);
        lv_obj_set_style_bg_opa(chip_[i], on[i] ? LV_OPA_COVER : 13, 0);
        set_color(chip_l_[i], on[i] ? hex(0x0b0c0d) : kFaint);
      }
    }
    set_text(chg_, !f ? dash : d.charging == 1 ? "Charging" : d.charging == 0 ? "Not charging" : "Key off");
  }

 private:
  ArcFill *fuel_ = nullptr, *temp_ = nullptr;
  lv_obj_t *fuel_v_ = nullptr, *temp_v_ = nullptr, *oil_v_ = nullptr, *chg_ = nullptr;
  lv_obj_t *chip_[3] = {}, *chip_l_[3] = {};
  bool chip_on_[3] = {};
  Smooth fuel_s_, temp_s_;
};

}  // namespace

Face *make_modern(GaugeFace f) {
  switch (f) {
    case GaugeFace::Speedo:
      return new ModernSpeedo();
    case GaugeFace::Tach:
      return new ModernTach();
    case GaugeFace::Clock:
      return new ModernClock();
    case GaugeFace::Econ:
      return new ModernEcon();
    case GaugeFace::Media:
      return new ModernMedia();
    case GaugeFace::Right:
    default:
      return new ModernRight();
  }
}

}  // namespace gauge
