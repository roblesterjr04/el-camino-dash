#include <elc_gauges.h>

#include <stdio.h>
#include <string.h>

#include "face.h"

#if defined(ESP_PLATFORM)
#include <esp_timer.h>
static uint32_t now_us() { return (uint32_t)esp_timer_get_time(); }
#else
#include <chrono>
static uint32_t now_us() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
#endif

namespace gauge {

Face::~Face() {
  for (int i = 0; i < n_; i++) {
    delete shapes_[i];
  }
}

void fmt_mmss(char *out, size_t n, float s) {
  const int t = s > 0 ? (int)s : 0;
  snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

const char *media_title(const GaugeData &d) {
  if (!d.media_connected) {
    return "No phone";
  }
  return (d.media_title && d.media_title[0]) ? d.media_title : "Not playing";
}

const char *media_artist(const GaugeData &d) {
  if (!d.media_connected) {
    return "Connect over Bluetooth";
  }
  return (d.media_artist && d.media_artist[0]) ? d.media_artist : "";
}

int hour12(int h24) { return (h24 + 11) % 12 + 1; }

}  // namespace gauge

using namespace gauge;

namespace {

/** Painted dials, kept so switching back to a face is instant. */
struct DialCache {
  GaugeTheme theme;
  GaugeFace face;
  uint8_t variant;
  uint16_t *px;
  uint32_t used;
};
constexpr int kCacheSlots = 5;
DialCache s_cache[kCacheSlots] = {};
uint32_t s_use_counter = 0;

lv_obj_t *s_parent = nullptr;
int s_x = 0, s_y = 0, s_size = 0;
Geo s_geo;

lv_obj_t *s_root = nullptr;
lv_obj_t *s_dial = nullptr;
lv_img_dsc_t s_dsc;
Face *s_face = nullptr;
GaugeTheme s_theme = GaugeTheme::Classic;
GaugeFace s_face_id = GaugeFace::Speedo;
uint8_t s_variant = 0;
bool s_have = false;
bool s_hidden = false;
uint32_t s_last_ms = 0;
uint32_t s_paint_ms = 0;

lv_obj_t *s_nodata = nullptr;  // dim layer + badge, shown while the link is stale

uint16_t *paint_dial(Face *f) {
  for (auto &e : s_cache) {
    if (e.px && e.theme == s_theme && e.face == s_face_id && e.variant == s_variant) {
      e.used = ++s_use_counter;
      return e.px;
    }
  }
  const uint32_t t0 = now_us();
  const size_t n = (size_t)s_size * (size_t)s_size;
  Canvas888 c;
  c.w = c.h = s_size;
  c.clip_x1 = c.clip_y1 = s_size;
  c.px = (uint8_t *)lv_mem_alloc(n * 3);
  if (!c.px) {
    return nullptr;
  }
  memset(c.px, 0, n * 3);
  Geo g;
  g.s = (float)s_size / 800.0f;
  f->paint(c, g);

  // Reuse the least recently used slot.
  DialCache *slot = &s_cache[0];
  for (auto &e : s_cache) {
    if (!e.px) {
      slot = &e;
      break;
    }
    if (e.used < slot->used) {
      slot = &e;
    }
  }
  if (!slot->px) {
    slot->px = (uint16_t *)lv_mem_alloc(n * 2);
  }
  if (slot->px) {
    to_rgb565(c, slot->px);
    slot->theme = s_theme;
    slot->face = s_face_id;
    slot->variant = s_variant;
    slot->used = ++s_use_counter;
  }
  lv_mem_free(c.px);
  s_paint_ms = (now_us() - t0) / 1000u;
  return slot->px;
}

void build_nodata(lv_obj_t *root) {
  s_nodata = lv_obj_create(root);
  lv_obj_remove_style_all(s_nodata);
  lv_obj_set_size(s_nodata, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(s_nodata, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_nodata, 140, 0);
  lv_obj_clear_flag(s_nodata, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_nodata, LV_OBJ_FLAG_HIDDEN);
  const Geo &g = s_geo;
  const float cx = g.X(400), cy = g.Y(560);
  if (s_theme == GaugeTheme::Modern) {
    lv_obj_t *pill = rect(s_nodata, cx - g.L(150), cy - g.L(30), g.L(300), g.L(60), hex(0xff3b30), g.L(30), 0.18f);
    (void)pill;
    lv_obj_t *l = label(s_nodata, &elc_sm_28, hex(0xff5a4e), cx, cy, g.L(300), Align::Center);
    lv_label_set_text(l, "NO SENSOR DATA");
    return;
  }
  const bool fo = s_theme == GaugeTheme::Fallout;
  const Rgb col = fo ? hex(0xffb347) : hex(0xff4a3d);
  lv_obj_t *box = rect(s_nodata, cx - g.L(156), cy - g.L(40), g.L(312), g.L(80), hex(0x000000), g.L(12), 0.0f);
  lv_obj_set_style_border_color(box, to_lv(col), 0);
  lv_obj_set_style_border_width(box, (lv_coord_t)fmaxf(2, g.L(4)), 0);
  lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
  lv_obj_t *l = label(s_nodata, fo ? &elc_vt_56 : &elc_bc_46, col, cx, cy, g.L(300), Align::Center);
  lv_label_set_text(l, "NO DATA");
}

}  // namespace

void gauges_begin(lv_obj_t *parent, int x, int y, int size) {
  s_parent = parent;
  s_x = x;
  s_y = y;
  s_size = size;
  s_geo.x0 = (float)x;
  s_geo.y0 = (float)y;
  s_geo.s = (float)size / 800.0f;
}

void gauges_show(GaugeTheme theme, GaugeFace face, uint8_t variant) {
  if (!s_parent) {
    return;
  }
  if (s_have && theme == s_theme && face == s_face_id && variant == s_variant) {
    return;
  }
  if (s_root) {
    lv_obj_del(s_root);
    s_root = nullptr;
    s_nodata = nullptr;
  }
  delete s_face;
  s_face = nullptr;

  s_theme = theme;
  s_face_id = face;
  s_variant = variant;
  s_have = true;
  if (theme == GaugeTheme::Modern) {
    s_face = make_modern(face);
  } else if (theme == GaugeTheme::Fallout) {
    s_face = make_fallout(face);
  } else {
    s_face = make_classic(face, variant);
  }

  s_root = lv_obj_create(s_parent);
  lv_obj_remove_style_all(s_root);
  lv_obj_set_pos(s_root, 0, 0);
  lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
  if (s_hidden) {
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  }

  uint16_t *px = paint_dial(s_face);
  s_dsc = {};
  s_dsc.header.always_zero = 0;
  s_dsc.header.w = (uint32_t)s_size;
  s_dsc.header.h = (uint32_t)s_size;
  s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  s_dsc.data_size = (uint32_t)s_size * (uint32_t)s_size * 2u;
  s_dsc.data = (const uint8_t *)px;
  s_dial = lv_img_create(s_root);
  if (px) {
    lv_img_set_src(s_dial, &s_dsc);
  }
  lv_obj_set_pos(s_dial, s_x, s_y);
  lv_obj_clear_flag(s_dial, LV_OBJ_FLAG_CLICKABLE);

  s_face->build(s_root, s_geo);
  build_nodata(s_root);
  s_last_ms = 0;
}

void gauges_update(const GaugeData &d, uint32_t now_ms) {
  if (!s_face) {
    return;
  }
  const float dt = s_last_ms ? (float)(now_ms - s_last_ms) / 1000.0f : 1.0f;
  s_last_ms = now_ms;
  s_face->update(d, dt > 0.25f ? 0.25f : dt);
  if (s_nodata) {
    show(s_nodata, s_face->uses_sensors() && !d.fresh);
  }
}

void gauges_set_hidden(bool hidden) {
  s_hidden = hidden;
  if (s_root) {
    show(s_root, !hidden);
  }
}

GaugeTheme gauges_theme() { return s_theme; }
GaugeFace gauges_face() { return s_face_id; }
uint32_t gauges_last_paint_ms() { return s_paint_ms; }
