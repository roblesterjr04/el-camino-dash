#include "ui_menu.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <accessory_can.h>
#include <chan_screen.h>
#include <elc_fonts.h>
#include <p4_screen.h>
#include <phone_can.h>
#include <ui_theme.h>

#if defined(ARDUINO)
#include <Arduino.h>
#else
extern "C" uint32_t millis(void);
#endif

// Material Icons code points as UTF-8
#define IC_SPEED "\xEE\xA7\xA4"
#define IC_CLOCK "\xEE\xA2\xB5"
#define IC_FUEL "\xEE\x95\x86"
#define IC_MUSIC "\xEE\x90\x85"
#define IC_PHONE "\xEE\x8C\xAC"
#define IC_BRIGHT "\xEE\x8E\xAB"
#define IC_PALETTE "\xEE\x90\x8A"
#define IC_SETTINGS "\xEE\xA2\xB8"
#define IC_BUILD "\xEE\xA1\xA9"
#define IC_BACK "\xEE\x97\x84"
#define IC_CLOSE "\xEE\x97\x8D"
#define IC_MINUS "\xEE\x85\x9B"
#define IC_PLUS "\xEE\x85\x85"
#define IC_BLUETOOTH "\xEE\x86\xA7"

namespace {

constexpr int kW = 800;
constexpr int kH = 800;
constexpr int kItemGap = 130;   // wheel spacing
constexpr float kCurveR = 490;  // wheel curvature (px)

const UiMenuHost *H = nullptr;

using Theme = UiTheme;
Theme theme_for(uint8_t t) { return ui_theme(t); }

uint8_t host_theme() { return H && H->theme ? H->theme() : 0; }

enum class Act : uint8_t { View, Page };
struct Item {
  const char *icon;
  const char *label;
  Act act;
  uint8_t arg;  // view (kUiView*) or UiMenuPage
};
const Item kItems[] = {
    {IC_SPEED, "Tachometer", Act::View, kUiViewTach},
    {IC_CLOCK, "Clock", Act::View, kUiViewClock},
    {IC_FUEL, "Efficiency", Act::View, kUiViewEcon},
    {IC_MUSIC, "Media", Act::View, kUiViewMedia},
    {IC_PHONE, "Phone", Act::Page, (uint8_t)UiMenuPage::Phone},
    {IC_PLUS, "Accessories", Act::Page, (uint8_t)UiMenuPage::Accessories},
    {IC_BRIGHT, "Brightness", Act::Page, (uint8_t)UiMenuPage::Brightness},
    {IC_PALETTE, "Theme", Act::Page, (uint8_t)UiMenuPage::Theme},
    {IC_SETTINGS, "Screens", Act::Page, (uint8_t)UiMenuPage::Screens},
    {IC_BUILD, "Service", Act::Page, (uint8_t)UiMenuPage::Service},
};
constexpr int kNumItems = sizeof(kItems) / sizeof(kItems[0]);

struct WheelItem {
  lv_obj_t *box, *ring, *icon, *label, *sub;
  int8_t big;   // -1 unknown, 0 neighbour style, 1 centred style
  int8_t tone;  // -1 unknown, else cached colour state (setting a style repaints the object)
  bool hidden;
  lv_coord_t x, y;
};

Theme T;
uint8_t s_theme_built = 0xFF;
lv_obj_t *s_root = nullptr;  // full-screen menu backdrop
lv_obj_t *s_page = nullptr;  // current page content
UiMenuPage s_page_id = UiMenuPage::Home;
lv_timer_t *s_timer = nullptr;

// wheel
WheelItem s_wheel[kNumItems];
lv_obj_t *s_dots[kNumItems];
float s_pos = 0.0f;  // animated position (item index)
int s_target = 0;
bool s_dragging = false;
lv_coord_t s_press_y = 0;
float s_press_pos = 0.0f;
bool s_dots_valid = false;
int s_dots_for = -1;

// pages
lv_obj_t *s_bright_arc = nullptr, *s_bright_val = nullptr, *s_follow_sw = nullptr;
uint8_t s_level = 100;
bool s_follow = true;
lv_obj_t *s_svc_val[kUiServiceRows];
lv_obj_t *s_svc_dot[kUiServiceRows];
lv_obj_t *s_demo_btn_lbl = nullptr;
lv_obj_t *s_screen_btn[2][3];  // [menu / media][left, center, right]
int s_screens_sig = -1;

// hold ring on the gauge screen
lv_obj_t *s_hold_arc = nullptr, *s_hold_lbl = nullptr;

// Phone and Accessories page state, defined with the pages below
const char *ph_home_sub();
const char *ac_home_sub();

// ---------------------------------------------------------------- small builders
/** Plain rectangle. Not clickable unless marked (LVGL objects are clickable by default). */
lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_style_all(l);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  return l;
}

void fill(lv_obj_t *o, lv_color_t c, lv_opa_t opa = LV_OPA_COVER) {
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
}

void outline(lv_obj_t *o, lv_color_t c, int w) {
  lv_obj_set_style_border_color(o, c, 0);
  lv_obj_set_style_border_width(o, w, 0);
  lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
}

/** Round icon button; pressed state darkens and shrinks slightly. */
lv_obj_t *round_btn(lv_obj_t *parent, const char *icon, int cx, int cy, int d, lv_event_cb_t cb, void *ud = nullptr) {
  lv_obj_t *b = box(parent, cx - d / 2, cy - d / 2, d, d);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  fill(b, T.surface);
  outline(b, T.line, 2);
  lv_obj_set_style_bg_color(b, T.line, LV_STATE_PRESSED);
  lv_obj_set_style_transform_zoom(b, 236, LV_STATE_PRESSED);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(b, 12);
  lv_obj_t *l = label(b, icon, &elc_icons_34, T.text);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

/** Pill-shaped row button with text. */
lv_obj_t *pill(lv_obj_t *parent, int x, int y, int w, int h, const char *text, lv_event_cb_t cb, void *ud = nullptr) {
  lv_obj_t *b = box(parent, x, y, w, h);
  lv_obj_set_style_radius(b, h / 2, 0);
  fill(b, T.surface);
  outline(b, T.line, 2);
  lv_obj_set_style_bg_color(b, T.line, LV_STATE_PRESSED);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *l = label(b, text, T.sub, T.text);
  lv_obj_center(l);
  if (cb) {
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  }
  return b;
}

lv_obj_t *page_title(lv_obj_t *parent, const char *text) {
  lv_obj_t *l = label(parent, text, T.title, T.text);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 104);
  return l;
}

void fade_in(lv_obj_t *o, uint32_t ms) {
  lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_time(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, [](void *v, int32_t x) { lv_obj_set_style_opa((lv_obj_t *)v, (lv_opa_t)x, 0); });
  lv_anim_start(&a);
}

void back_home(lv_event_t *) { ui_menu_back(); }

void add_back(lv_obj_t *page) { round_btn(page, IC_BACK, kW / 2, 716, 76, back_home); }

// ---------------------------------------------------------------- home wheel
uint8_t media_screen() { return H->media_screen ? H->media_screen() : kUiScreenCenter; }
uint8_t center_view() { return H->center_view ? H->center_view() : kUiViewTach; }

/** The item's view is on screen now (Media: wherever the media screen is). */
bool item_showing(const Item &it) {
  if (it.act != Act::View) {
    return false;
  }
  if (it.arg == kUiViewMedia && media_screen() != kUiScreenCenter) {
    return H->media_shown && H->media_shown();
  }
  return center_view() == it.arg;
}

const char *item_sub(int i) {
  const Item &it = kItems[i];
  static char buf[48];
  if (it.act == Act::View) {
    if (item_showing(it)) {
      return "Showing now";
    }
    switch (it.arg) {
      case kUiViewTach: return "Engine RPM";
      case kUiViewClock: return "Time and date";
      case kUiViewEcon: return "MPG and range";
      default:
        if (media_screen() != kUiScreenCenter) {
          snprintf(buf, sizeof(buf), "On the %s screen", media_screen() == kUiScreenLeft ? "left" : "right");
          return buf;
        }
        return "Bluetooth audio";
    }
  }
  switch ((UiMenuPage)it.arg) {
    case UiMenuPage::Brightness:
      snprintf(buf, sizeof(buf), "%u%%%s", (unsigned)s_level, s_follow ? " \xC2\xB7 dims with lights" : "");
      return buf;
    case UiMenuPage::Theme: {
      const uint8_t th = host_theme();
      return th == 1 ? "Fallout" : (th == 2 ? "Modern" : "Classic");
    }
    case UiMenuPage::Screens: {
      static const char *const kLower[3] = {"left", "center", "right"};
      const uint8_t m = H->interface_screen ? H->interface_screen() : kUiScreenCenter;
      snprintf(buf, sizeof(buf), "Menu %s \xC2\xB7 media %s", kLower[m > 2 ? 1 : m], kLower[media_screen() > 2 ? 1 : media_screen()]);
      return buf;
    }
    case UiMenuPage::Phone: return ph_home_sub();
    case UiMenuPage::Accessories: return ac_home_sub();
    default: return "Diagnostics";
  }
}

void activate(int i) {
  const Item &it = kItems[i];
  if (it.act == Act::View) {
    if (it.arg == kUiViewMedia && media_screen() != kUiScreenCenter) {
      if (H->show_media) H->show_media(true);
    } else if (H->set_center_view) {
      H->set_center_view(it.arg);
    }
    ui_menu_close();
  } else {
    ui_menu_open((UiMenuPage)it.arg);
  }
}

void layout_wheel() {
  for (int i = 0; i < kNumItems; i++) {
    WheelItem &w = s_wheel[i];
    const float off = (float)i - s_pos;
    const float ao = fabsf(off);
    if (ao > 1.6f) {  // centred item + one neighbour each side: less to repaint while scrolling
      if (!w.hidden) {
        w.hidden = true;
        lv_obj_add_flag(w.box, LV_OBJ_FLAG_HIDDEN);
      }
      continue;
    }
    if (w.hidden) {
      w.hidden = false;
      lv_obj_clear_flag(w.box, LV_OBJ_FLAG_HIDDEN);
    }
    const float dy = off * (float)kItemGap;
    const float xs = fabsf(dy) < kCurveR ? kCurveR - sqrtf(kCurveR * kCurveR - dy * dy) : kCurveR;
    const lv_coord_t nx = (lv_coord_t)(170 + xs), ny = (lv_coord_t)(kH / 2 - 60 + dy);
    if (nx != w.x || ny != w.y) {
      w.x = nx;
      w.y = ny;
      lv_obj_set_pos(w.box, nx, ny);
    }
    // No transforms or group opacity here: those force per-item layer renders in software and made
    // dragging choppy. The centred item uses the big font and ring; neighbours are smaller and dimmer.
    const bool big = ao < 0.5f;
    const bool sel = (i == s_target);
    if (w.big != big) {
      w.big = big;
      lv_obj_set_style_text_font(w.label, big ? T.label : T.body, 0);
      lv_obj_set_size(w.ring, big ? 100 : 72, big ? 100 : 72);
      lv_obj_set_pos(w.ring, big ? 0 : 14, big ? 10 : 24);
      lv_obj_set_style_text_font(w.icon, big ? &elc_icons_56 : &elc_icons_34, 0);
      lv_obj_align(w.label, LV_ALIGN_LEFT_MID, big ? 124 : 104, big ? -16 : 0);
      if (big) lv_obj_clear_flag(w.sub, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(w.sub, LV_OBJ_FLAG_HIDDEN);
    }
    const bool showing = item_showing(kItems[i]);
    const int8_t tone = (int8_t)((sel ? 1 : 0) | (ao < 1.5f ? 2 : 0) | (showing ? 4 : 0));
    if (tone == w.tone) {
      continue;
    }
    w.tone = tone;
    const lv_color_t fade = ao < 1.5f ? T.dim : T.faint;
    lv_obj_set_style_border_color(w.ring, sel ? T.accent : T.line, 0);
    lv_obj_set_style_border_width(w.ring, sel ? 3 : 2, 0);
    lv_obj_set_style_bg_opa(w.ring, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(w.icon, sel ? T.accent : fade, 0);
    lv_obj_set_style_text_color(w.label, sel ? T.text : fade, 0);
    lv_obj_set_style_text_color(w.sub, sel && showing ? T.accent : T.faint, 0);
  }
  if (s_dots_valid && s_dots_for == s_target) {
    return;
  }
  s_dots_valid = true;
  s_dots_for = s_target;
  for (int i = 0; i < kNumItems; i++) {
    const bool on = (i == s_target);
    const int d = on ? 14 : 8;
    const float a = (-24.0f + 48.0f * (float)i / (float)(kNumItems - 1)) * (float)M_PI / 180.0f;
    lv_obj_set_size(s_dots[i], d, d);
    lv_obj_set_pos(s_dots[i], (lv_coord_t)(kW / 2 + cosf(a) * 360 - d / 2), (lv_coord_t)(kH / 2 + sinf(a) * 360 - d / 2));
    fill(s_dots[i], on ? T.accent : T.line);
  }
}

void wheel_event(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) {
    return;
  }
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  if (code == LV_EVENT_PRESSED) {
    s_press_y = p.y;
    s_press_pos = s_pos;
    s_dragging = false;
  } else if (code == LV_EVENT_PRESSING) {
    const int dy = p.y - s_press_y;
    if (!s_dragging && abs(dy) > 14) {
      s_dragging = true;
    }
    if (s_dragging) {
      float pos = s_press_pos - (float)dy / (float)kItemGap;
      pos = fmaxf(-0.3f, fminf((float)(kNumItems - 1) + 0.3f, pos));
      s_pos = pos;
      s_target = (int)lroundf(fmaxf(0.0f, fminf((float)(kNumItems - 1), pos)));
      layout_wheel();
    }
  } else if (code == LV_EVENT_RELEASED) {
    if (s_dragging) {
      s_dragging = false;
      return;  // the timer eases s_pos to s_target
    }
    const int idx = (int)lroundf(s_pos + (float)(s_press_y - kH / 2) / (float)kItemGap);  // a tap: where it landed
    if (idx < 0 || idx >= kNumItems) {
      return;
    }
    // A tap on any visible item opens it (stepping one item at a time is for the knob).
    s_target = idx;
    s_pos = (float)idx;
    activate(idx);
  }
}

void build_home(lv_obj_t *page) {
  // Touch catcher under the items: drag to scroll, tap to select / open.
  lv_obj_t *catcher = box(page, 0, 0, kW, kH);
  lv_obj_add_flag(catcher, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(catcher, wheel_event, LV_EVENT_ALL, nullptr);

  lv_obj_t *t = label(page, "MENU", T.sub, T.faint);
  lv_obj_set_style_text_letter_space(t, 4, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 74);

  for (int i = 0; i < kNumItems; i++) {
    WheelItem &w = s_wheel[i];
    w.box = box(page, 0, 0, 470, 120);
    w.big = -1;
    w.tone = -1;
    w.hidden = false;
    w.x = w.y = -32000;
    w.ring = box(w.box, 0, 10, 100, 100);
    lv_obj_set_style_radius(w.ring, LV_RADIUS_CIRCLE, 0);
    fill(w.ring, T.surface, LV_OPA_TRANSP);
    outline(w.ring, T.line, 2);
    w.icon = label(w.ring, kItems[i].icon, &elc_icons_56, T.dim);
    lv_obj_center(w.icon);
    w.label = label(w.box, kItems[i].label, T.label, T.dim);
    lv_obj_align(w.label, LV_ALIGN_LEFT_MID, 124, -16);
    w.sub = label(w.box, item_sub(i), T.sub, T.faint);
    lv_label_set_long_mode(w.sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(w.sub, 340);
    lv_obj_align(w.sub, LV_ALIGN_LEFT_MID, 126, 30);
  }
  for (int i = 0; i < kNumItems; i++) {
    s_dots[i] = box(page, 0, 0, 8, 8);
    lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
  }
  round_btn(page, IC_CLOSE, kW / 2, 716, 72, [](lv_event_t *) { ui_menu_close(); });
  s_dots_valid = false;
  layout_wheel();
}

// ---------------------------------------------------------------- Brightness
void bright_apply(bool save) {
  if (H->set_brightness) H->set_brightness(s_level, s_follow, save);
  if (s_bright_val) {
    lv_label_set_text_fmt(s_bright_val, "%u%%", (unsigned)s_level);
  }
  if (s_bright_arc) {
    lv_arc_set_value(s_bright_arc, s_level);
  }
}

void build_brightness(lv_obj_t *page) {
  page_title(page, "Brightness");
  s_bright_arc = lv_arc_create(page);
  lv_obj_set_size(s_bright_arc, 360, 360);
  lv_obj_align(s_bright_arc, LV_ALIGN_CENTER, 0, 10);
  lv_arc_set_rotation(s_bright_arc, 135);
  lv_arc_set_bg_angles(s_bright_arc, 0, 270);
  lv_arc_set_range(s_bright_arc, 10, 100);
  lv_arc_set_value(s_bright_arc, s_level);
  lv_obj_set_style_arc_width(s_bright_arc, 26, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_bright_arc, T.line, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_bright_arc, 26, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_bright_arc, T.accent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_bright_arc, T.text, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s_bright_arc, 4, LV_PART_KNOB);
  lv_obj_add_event_cb(s_bright_arc, [](lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
    s_level = (uint8_t)lv_arc_get_value(lv_event_get_target(e));
    bright_apply(code == LV_EVENT_RELEASED);
  }, LV_EVENT_ALL, nullptr);
  s_bright_val = label(page, "", T.label, T.text);
  lv_obj_align(s_bright_val, LV_ALIGN_CENTER, 0, 0);
  lv_obj_t *cap = label(page, "all three displays", T.sub, T.faint);
  lv_obj_align(cap, LV_ALIGN_CENTER, 0, 52);
  round_btn(page, IC_MINUS, 132, 410, 76, [](lv_event_t *) {
    s_level = (uint8_t)((s_level > 20) ? s_level - 10 : 10);
    bright_apply(true);
  });
  round_btn(page, IC_PLUS, kW - 132, 410, 76, [](lv_event_t *) {
    s_level = (uint8_t)((s_level < 90) ? s_level + 10 : 100);
    bright_apply(true);
  });
  lv_obj_t *row = pill(page, 230, 612, 340, 58, "", [](lv_event_t *) {
    s_follow = !s_follow;
    if (s_follow) lv_obj_add_state(s_follow_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(s_follow_sw, LV_STATE_CHECKED);
    bright_apply(true);
  });
  lv_obj_t *rl = lv_obj_get_child(row, 0);
  lv_label_set_text(rl, "Dim with headlights");
  lv_obj_align(rl, LV_ALIGN_LEFT_MID, 24, 0);
  s_follow_sw = lv_switch_create(row);
  lv_obj_set_size(s_follow_sw, 64, 34);
  lv_obj_align(s_follow_sw, LV_ALIGN_RIGHT_MID, -14, 0);
  lv_obj_clear_flag(s_follow_sw, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(s_follow_sw, T.line, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_follow_sw, T.accent, (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(s_follow_sw, T.text, LV_PART_KNOB);
  if (s_follow) lv_obj_add_state(s_follow_sw, LV_STATE_CHECKED);
  add_back(page);
  if (s_bright_val) lv_label_set_text_fmt(s_bright_val, "%u%%", (unsigned)s_level);
}

// ---------------------------------------------------------------- Theme
void theme_pick(lv_event_t *e) {
  const uint8_t t = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  if (host_theme() == t || !H->set_theme) {
    return;
  }
  H->set_theme(t);  // the page rebuilds in the new theme once the change arrives (tick)
}

void build_theme(lv_obj_t *page) {
  page_title(page, "Theme");
  struct Opt {
    uint8_t id;
    const char *name;
  };
  const Opt opts[] = {{0, "Classic"}, {2, "Modern"}, {1, "Fallout"}};
  const int n = sizeof(opts) / sizeof(opts[0]);
  for (int i = 0; i < n; i++) {
    const int cx = kW / 2 + (i - (n - 1) / 2.0f) * 225;
    const Theme pt = theme_for(opts[i].id);
    const bool on = host_theme() == opts[i].id;
    lv_obj_t *ring = box(page, cx - 100, 262, 200, 200);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    outline(ring, on ? T.accent : T.line, on ? 5 : 2);
    lv_obj_add_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_zoom(ring, 240, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ring, theme_pick, LV_EVENT_CLICKED, (void *)(intptr_t)opts[i].id);
    lv_obj_t *face = box(ring, 12, 12, 176, 176);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    fill(face, pt.bg);
    outline(face, pt.line, 2);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *big = label(face, "Aa", pt.label, pt.text);
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *bar = box(face, 43, 122, 90, 8);
    lv_obj_set_style_radius(bar, 4, 0);
    fill(bar, pt.accent);
    lv_obj_t *nm = label(page, opts[i].name, T.body, on ? T.text : T.dim);
    lv_obj_align(nm, LV_ALIGN_TOP_MID, cx - kW / 2, 494);
  }
  lv_obj_t *cap = label(page, "Applies to all three displays.\nHold a screen 10 s to switch anywhere.", T.sub, T.faint);
  lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 560);
  add_back(page);
}

// ---------------------------------------------------------------- Screens
int screens_sig() {
  return (H->interface_screen ? H->interface_screen() : kUiScreenCenter) * 4 + media_screen();
}

void screens_restyle() {
  const uint8_t cur[2] = {H->interface_screen ? H->interface_screen() : (uint8_t)kUiScreenCenter, media_screen()};
  for (int r = 0; r < 2; r++) {
    for (int s = 0; s < 3; s++) {
      lv_obj_t *b = s_screen_btn[r][s];
      if (!b) continue;
      const bool on = cur[r] == s;
      fill(b, on ? T.accent : T.surface);
      outline(b, on ? T.accent : T.line, 2);
      lv_obj_set_style_text_color(lv_obj_get_child(b, 0), on ? T.ink : T.text, 0);
    }
  }
  s_screens_sig = screens_sig();
}

void screen_pick(lv_event_t *e) {
  const int v = (int)(intptr_t)lv_event_get_user_data(e);
  const uint8_t row = (uint8_t)(v >> 4), screen = (uint8_t)(v & 3);
  if (row == 0 && H->set_interface_screen) H->set_interface_screen(screen);
  if (row == 1 && H->set_media_screen) H->set_media_screen(screen);
}

void build_screens(lv_obj_t *page) {
  page_title(page, "Screens");
  static const char *const kRows[2] = {"MENU", "MEDIA"};
  static const char *const kNames[3] = {"Left", "Center", "Right"};
  for (int r = 0; r < 2; r++) {
    const int y = 196 + r * 150;
    lv_obj_t *t = label(page, kRows[r], T.sub, T.faint);
    lv_obj_set_style_text_letter_space(t, 4, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, y);
    for (int s = 0; s < 3; s++) {
      s_screen_btn[r][s] = pill(page, 158 + s * 166, y + 40, 150, 62, kNames[s], screen_pick, (void *)(intptr_t)((r << 4) | s));
    }
  }
  lv_obj_t *cap = label(page,
                        "The menu moves to the chosen screen; the\n"
                        "knob drives it there. Media is an extra view\n"
                        "on its screen: hold 3 s to switch to it.",
                        T.sub, T.faint);
  lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 510);
  add_back(page);
  screens_restyle();
}

// ---------------------------------------------------------------- Service
const char *const kSvcNames[kUiServiceRows] = {"Sensor link", "Odometer", "Faults", "GPS", "Audio bridge", "Left screen", "Right screen"};

void svc_refresh() {
  for (int i = 0; i < kUiServiceRows; i++) {
    if (!s_svc_val[i]) continue;
    char v[48] = "";
    int level = 1;
    if (H->service_row) H->service_row(i, v, sizeof(v), &level);
    if (strcmp(lv_label_get_text(s_svc_val[i]), v) != 0) lv_label_set_text(s_svc_val[i], v);
    lv_obj_set_style_text_color(s_svc_val[i], level == 2 ? T.bad : (level == 0 ? T.text : T.dim), 0);
    fill(s_svc_dot[i], level == 0 ? T.ok : (level == 1 ? T.faint : T.bad));
  }
  if (s_demo_btn_lbl) {
    lv_label_set_text(s_demo_btn_lbl, H->demo && H->demo() ? "Demo: on" : "Demo: off");
  }
}

void build_service(lv_obj_t *page) {
  page_title(page, "Service");
  for (int i = 0; i < kUiServiceRows; i++) {
    const int y = 176 + i * 50;
    s_svc_dot[i] = box(page, 162, y + 19, 12, 12);
    lv_obj_set_style_radius(s_svc_dot[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_t *n = label(page, kSvcNames[i], T.sub, T.text);
    lv_obj_set_pos(n, 188, y + 10);
    s_svc_val[i] = label(page, "", T.sub, T.dim);
    lv_label_set_long_mode(s_svc_val[i], LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_svc_val[i], 230, lv_font_get_line_height(T.sub));  // one line, ellipsis
    lv_obj_set_style_text_align(s_svc_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_svc_val[i], 410, y + 10);
    lv_obj_t *rule = box(page, 156, y + 48, 488, 1);
    fill(rule, T.line);
  }
  pill(page, 150, 598, 160, 56, "Sensor log", [](lv_event_t *) {
    ui_menu_close();
    if (H->open_center_page) H->open_center_page(2);
  });
  pill(page, 320, 598, 160, 56, "GPS data", [](lv_event_t *) {
    ui_menu_close();
    if (H->open_center_page) H->open_center_page(3);
  });
  lv_obj_t *demo = pill(page, 490, 598, 160, 56, "Demo", [](lv_event_t *) {
    if (H->set_demo && H->demo) H->set_demo(!H->demo());
    svc_refresh();
  });
  s_demo_btn_lbl = lv_obj_get_child(demo, 0);
  add_back(page);
  svc_refresh();
}

// ---------------------------------------------------------------- Phone
// Pairing, the paired list, voice assistant and redial, from the audio bridge's phone state (phone_can.h;
// docs/protocols/can_bus.md, "Phone"). The body is rebuilt from tick (never inside a click handler) when what it
// shows changes; the status card and the pairing countdown update in place. The knob moves a focus ring over the
// choices and presses the focused one; touch presses one directly.
enum PhAct : uint8_t {
  kPhPairYes, kPhPairNo, kPhPairNew, kPhStop, kPhPhone, kPhVoice, kPhRedial,
  kPhConnect, kPhDisconnect, kPhForget, kPhForgetYes, kPhForgetNo,
};
enum class PhView : uint8_t { Gone, Confirm, List, Detail, Forget };

constexpr int kPhMaxItems = 16;
constexpr int kPhW = 460;  // body width (x 170..630)
constexpr int kPhRowH = 60, kPhGap = 10;
const char *const kPhBtName = "ElCamino-Media";  // the bridge's Bluetooth name (BT_DEVICE_NAME in the bridge sketch)

struct PhItem {
  uint8_t act, arg;
  lv_obj_t *obj;
  char label[40];
};
PhItem s_ph_items[kPhMaxItems];
int s_ph_n = 0;
int s_ph_focus = 0;
uint8_t s_ph_focus_act = 0xFF, s_ph_focus_arg = 0;  // the focus, kept across rebuilds by what it does
lv_obj_t *s_ph_st1 = nullptr, *s_ph_st2 = nullptr, *s_ph_body = nullptr, *s_ph_left = nullptr;
uint32_t s_ph_sig = 0, s_ph_poll_ms = 0;
bool s_ph_dirty = false;
uint8_t s_ph_sel = 0xFF, s_ph_sel_serial = 0;  // the phone chosen in the list (index, and the list serial it was read with)
bool s_ph_forget = false;                      // asking to confirm Forget

/** Cut s to at most max bytes without splitting a UTF-8 sequence. */
void utf8_cut(char *s, size_t max) {
  if (strlen(s) <= max) return;
  size_t k = max;
  while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
  s[k] = '\0';
}

const ElcCanPhoneStatus &ph_st() { return phone_can_status(); }
bool ph_connected() { return phone_can_heard() && (ph_st().flags & ELC_CAN_PHONE_CONNECTED); }
bool ph_hfp() { return ph_connected() && (ph_st().flags & ELC_CAN_PHONE_HFP); }

/** Paired phone i's name, or "Phone n" until its name has arrived. */
const char *ph_paired(uint8_t i, char *buf, size_t n) {
  const char *nm = phone_can_paired_name(i);
  if (nm[0]) return nm;
  snprintf(buf, n, "Phone %u", (unsigned)i + 1u);
  return buf;
}

const char *ph_connected_name() {
  const char *nm = phone_can_phone_name();
  if (nm[0]) return nm;
  const uint8_t ci = ph_st().connected_index;
  nm = ci < ph_st().paired ? phone_can_paired_name(ci) : "";
  return nm[0] ? nm : "Phone";
}

void ph_code(char *out, size_t n) {
  uint32_t c = 0;
  if (phone_can_passkey(&c)) {
    snprintf(out, n, "%03u %03u", (unsigned)(c / 1000u), (unsigned)(c % 1000u));  // leading zeros kept
  } else {
    snprintf(out, n, "--- ---");
  }
}

/** "1:45 left" / "until a phone pairs", "" while the pairing window is closed. */
void ph_left(char *out, size_t n) {
  uint8_t left = 255;
  if (!phone_can_pairing_window(&left)) {
    out[0] = '\0';
  } else if (left == 255) {
    snprintf(out, n, "until a phone pairs");
  } else {
    snprintf(out, n, "%u:%02u left", (unsigned)(left / 60u), (unsigned)(left % 60u));
  }
}

PhView ph_view() {
  if (!phone_can_heard()) return PhView::Gone;
  const ElcCanPhoneStatus &st = ph_st();
  if (st.flags & ELC_CAN_PHONE_CONFIRM) return PhView::Confirm;  // a code to confirm comes first
  if (s_ph_sel != 0xFF && (st.list_serial != s_ph_sel_serial || s_ph_sel >= st.paired)) {
    s_ph_sel = 0xFF;  // the list changed: that index may be another phone now
    s_ph_forget = false;
  }
  if (s_ph_sel == 0xFF) return PhView::List;
  return s_ph_forget ? PhView::Forget : PhView::Detail;
}

/** Status card: who is connected, signal and battery. */
void ph_status(char *l1, size_t n1, char *l2, size_t n2) {
  const ElcCanPhoneStatus &st = ph_st();
  if (!phone_can_heard()) {
    snprintf(l1, n1, "Audio bridge not heard");
    snprintf(l2, n2, "Phones connect through the audio bridge");
  } else if (st.flags & ELC_CAN_PHONE_CONNECTED) {
    snprintf(l1, n1, "%s", ph_connected_name());
    char sig[24] = "", bat[24] = "";
    if (st.signal <= 5) snprintf(sig, sizeof(sig), "Signal %u/5", (unsigned)st.signal);
    if (st.battery <= 5) snprintf(bat, sizeof(bat), "Battery %u/5", (unsigned)st.battery);
    const char *what = (st.flags & ELC_CAN_PHONE_VOICE) ? "Voice assistant on"
                       : (st.flags & ELC_CAN_PHONE_HFP) ? "Hands-free"
                                                        : "Audio only";
    if (sig[0] || bat[0]) {
      snprintf(l2, n2, "%s%s%s \xC2\xB7 %s", sig, sig[0] && bat[0] ? " \xC2\xB7 " : "", bat, what);
    } else {
      snprintf(l2, n2, "%s", what);
    }
  } else {
    snprintf(l1, n1, "No phone connected");
    if (st.paired) {
      snprintf(l2, n2, "%u paired \xC2\xB7 choose one to connect", (unsigned)st.paired);
    } else {
      snprintf(l2, n2, "Pair a phone to start");
    }
  }
}

/** One line for the console and the wheel: the state that matters most. */
void ph_summary(char *out, size_t n) {
  const ElcCanPhoneStatus &st = ph_st();
  char left[24];
  ph_left(left, sizeof(left));
  if (!phone_can_heard()) {
    snprintf(out, n, "Audio bridge not heard");
  } else if (st.flags & ELC_CAN_PHONE_CONFIRM) {
    char code[12];
    ph_code(code, sizeof(code));
    // Keep the code whole within the console's 32 bytes: shorten the name instead.
    char pn[24];
    snprintf(pn, sizeof(pn), "%s", phone_can_pairing_name()[0] ? phone_can_pairing_name() : "phone");
    const int room = 32 - (int)strlen("Pair with ? ") - (int)strlen(code);
    utf8_cut(pn, room > 0 ? (size_t)room : 0);
    snprintf(out, n, "Pair with %s? %s", pn, code);
  } else if (left[0]) {
    snprintf(out, n, "Discoverable \xC2\xB7 %s", left);
  } else if (st.flags & ELC_CAN_PHONE_CONNECTED) {
    snprintf(out, n, "Connected \xC2\xB7 %s", ph_connected_name());
  } else {
    snprintf(out, n, "No phone connected");
  }
}

const char *ph_home_sub() {
  static char buf[48];
  const ElcCanPhoneStatus &st = ph_st();
  if (!phone_can_heard()) return "Audio bridge not heard";
  if (st.flags & ELC_CAN_PHONE_CONFIRM) return "Pairing request";
  if (st.flags & ELC_CAN_PHONE_CONNECTED) {
    snprintf(buf, sizeof(buf), "%s", ph_connected_name());
    return buf;
  }
  if (st.flags & ELC_CAN_PHONE_PAIRING) return "Discoverable";
  return "Not connected";
}

uint32_t fnv(uint32_t h, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}
uint32_t fnv_str(uint32_t h, const char *s) { return fnv(h, s, strlen(s) + 1); }

/** Everything the body shows (not the countdown, which updates in place). */
uint32_t ph_signature() {
  const PhView v = ph_view();
  uint32_t h = fnv(2166136261u, &v, sizeof(v));
  if (v == PhView::Gone) return h;
  const ElcCanPhoneStatus &st = ph_st();
  const uint8_t k[6] = {(uint8_t)(st.flags & (ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP | ELC_CAN_PHONE_PAIRING |
                                              ELC_CAN_PHONE_CONFIRM | ELC_CAN_PHONE_VOICE)),
                        st.paired, st.connected_index, st.list_serial, s_ph_sel, (uint8_t)s_ph_forget};
  h = fnv(h, k, sizeof(k));
  for (uint8_t i = 0; i < st.paired && i < ELC_CAN_PHONE_MAX_PAIRED; i++) h = fnv_str(h, phone_can_paired_name(i));
  if (v == PhView::Confirm) {
    char code[12];
    ph_code(code, sizeof(code));
    h = fnv_str(h, code);
    h = fnv_str(h, phone_can_pairing_name());
  }
  return h;
}

void ph_style_focus(int i, bool on) {
  if (i < 0 || i >= s_ph_n || !s_ph_items[i].obj) return;
  lv_obj_set_style_border_color(s_ph_items[i].obj, on ? T.accent : T.line, 0);
  lv_obj_set_style_border_width(s_ph_items[i].obj, on ? 3 : 2, 0);
}

void ph_set_focus(int i, bool scroll) {
  if (s_ph_n == 0) return;
  if (i < 0) i = 0;
  if (i >= s_ph_n) i = s_ph_n - 1;
  if (i != s_ph_focus) ph_style_focus(s_ph_focus, false);
  s_ph_focus = i;
  ph_style_focus(i, true);
  s_ph_focus_act = s_ph_items[i].act;
  s_ph_focus_arg = s_ph_items[i].arg;
  if (scroll) lv_obj_scroll_to_view(s_ph_items[i].obj, LV_ANIM_ON);
}

void ph_activate(int i) {
  if (i < 0 || i >= s_ph_n) return;
  const PhItem it = s_ph_items[i];
  const ElcCanPhoneStatus &st = ph_st();
  uint8_t fa = it.act, fg = it.arg;  // where the focus goes after the rebuild
  switch (it.act) {
    case kPhPairYes: phone_can_send(ELC_CAN_PHONE_CMD_CONFIRM, 1); break;
    case kPhPairNo: phone_can_send(ELC_CAN_PHONE_CMD_CONFIRM, 0); break;
    case kPhPairNew: phone_can_send(ELC_CAN_PHONE_CMD_PAIRING, 1); fa = kPhStop; break;
    case kPhStop: phone_can_send(ELC_CAN_PHONE_CMD_PAIRING, 0); fa = kPhPairNew; break;
    case kPhVoice: phone_can_send(ELC_CAN_PHONE_CMD_VOICE, (st.flags & ELC_CAN_PHONE_VOICE) ? 0 : 1); break;
    case kPhRedial: phone_can_send(ELC_CAN_PHONE_CMD_REDIAL, 0); break;
    case kPhPhone:
      s_ph_sel = it.arg;
      s_ph_sel_serial = st.list_serial;
      s_ph_forget = false;
      fa = (ph_connected() && st.connected_index == it.arg) ? kPhDisconnect : kPhConnect;
      fg = 0;
      break;
    case kPhConnect:
    case kPhDisconnect:
      if (it.act == kPhConnect) {
        phone_can_send(ELC_CAN_PHONE_CMD_CONNECT, s_ph_sel);
      } else {
        phone_can_send(ELC_CAN_PHONE_CMD_DISCONNECT, 0);
      }
      fa = kPhPhone;
      fg = s_ph_sel;
      s_ph_sel = 0xFF;  // back to the list, where the connected mark moves once the bridge reports it
      break;
    case kPhForget: s_ph_forget = true; fa = kPhForgetNo; break;
    case kPhForgetYes:
      phone_can_send(ELC_CAN_PHONE_CMD_FORGET, s_ph_sel);
      s_ph_sel = 0xFF;
      s_ph_forget = false;
      fa = kPhPairNew;
      fg = 0;
      break;
    case kPhForgetNo: s_ph_forget = false; fa = kPhForget; fg = 0; break;
    default: break;
  }
  s_ph_focus_act = fa;
  s_ph_focus_arg = fg;
  s_ph_dirty = true;  // tick rebuilds (this may be running inside the clicked object's handler)
}

void ph_clicked(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_ph_n) return;
  ph_set_focus(i, false);
  ph_activate(i);
}

lv_obj_t *ph_add(lv_obj_t *o, uint8_t act, uint8_t arg, const char *text) {
  if (s_ph_n >= kPhMaxItems) return o;
  PhItem &it = s_ph_items[s_ph_n];
  it.act = act;
  it.arg = arg;
  it.obj = o;
  snprintf(it.label, sizeof(it.label), "%s", text);
  lv_obj_add_event_cb(o, ph_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)s_ph_n);
  s_ph_n++;
  return o;
}

/** A centred pill choice. */
lv_obj_t *ph_button(int x, int y, int w, const char *text, lv_color_t color, uint8_t act, uint8_t arg = 0) {
  lv_obj_t *b = pill(s_ph_body, x, y, w, 62, text, nullptr);
  lv_obj_set_style_text_color(lv_obj_get_child(b, 0), color, 0);
  return ph_add(b, act, arg, text);
}

/** A list row: icon, text, and a note on the right. */
lv_obj_t *ph_row(int y, const char *icon, lv_color_t icon_color, const char *text, const char *note, lv_color_t note_color,
                 uint8_t act, uint8_t arg = 0) {
  lv_obj_t *row = box(s_ph_body, 0, y, kPhW, kPhRowH);
  lv_obj_set_style_radius(row, 18, 0);
  fill(row, T.surface);
  outline(row, T.line, 2);
  lv_obj_set_style_bg_color(row, T.line, LV_STATE_PRESSED);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  int x = 22;
  if (icon) {
    lv_obj_t *ic = label(row, icon, &elc_icons_34, icon_color);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
    x = 66;
  }
  const int note_w = note ? 130 : 0;
  lv_obj_t *l = label(row, text, T.sub, T.text);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, kPhW - x - note_w - 20);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 0);
  if (note) {
    lv_obj_t *nl = label(row, note, T.sub, note_color);
    lv_obj_align(nl, LV_ALIGN_RIGHT_MID, -20, 0);
  }
  return ph_add(row, act, arg, text);
}

lv_obj_t *ph_text(const char *text, const lv_font_t *font, lv_color_t color, int y, bool wrap = false) {
  lv_obj_t *l = label(s_ph_body, text, font, color);
  lv_obj_set_width(l, kPhW);
  lv_label_set_long_mode(l, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(l, 0, y);
  return l;
}

const lv_font_t *ph_code_font() {
  const uint8_t th = host_theme();
  return th == 1 ? &elc_vt_120d : (th == 2 ? &elc_sa_136d : &elc_bc_64d);
}

void ph_refresh_status() {
  if (!s_ph_st1) return;
  char l1[64], l2[80];
  ph_status(l1, sizeof(l1), l2, sizeof(l2));
  if (strcmp(lv_label_get_text(s_ph_st1), l1) != 0) lv_label_set_text(s_ph_st1, l1);
  if (strcmp(lv_label_get_text(s_ph_st2), l2) != 0) lv_label_set_text(s_ph_st2, l2);
  if (s_ph_left) {
    char left[24];
    ph_left(left, sizeof(left));
    if (strcmp(lv_label_get_text(s_ph_left), left) != 0) lv_label_set_text(s_ph_left, left);
  }
}

void ph_build_body() {
  if (!s_ph_body) return;
  lv_obj_clean(s_ph_body);
  lv_obj_scroll_to_y(s_ph_body, 0, LV_ANIM_OFF);
  s_ph_n = 0;
  s_ph_focus = 0;
  s_ph_left = nullptr;
  const PhView v = ph_view();
  const ElcCanPhoneStatus &st = ph_st();
  int def = 0;  // focus when the kept one is gone
  char buf[80], nb[16];
  switch (v) {
    case PhView::Gone:
      ph_text("The phone links through the audio\nbridge, which is not on the bus.", T.sub, T.faint, 60, true);
      break;
    case PhView::Confirm: {
      const char *pn = phone_can_pairing_name();
      if (pn[0]) {
        snprintf(buf, sizeof(buf), "Pair with %s?", pn);
      } else {
        snprintf(buf, sizeof(buf), "Pair a new phone?");
      }
      ph_text(buf, T.body, T.text, 0);
      char code[12];
      ph_code(code, sizeof(code));
      const lv_font_t *cf = ph_code_font();
      const int ch = lv_font_get_line_height(cf);
      const int cy = 44 + (170 - ch) / 2;  // centred in the band between the question and the hint
      ph_text(code, cf, T.accent, cy);
      ph_text("Check that the phone shows the same code", T.sub, T.dim, 222);
      ph_button(0, 280, 222, "Reject", T.bad, kPhPairNo);
      ph_button(238, 280, 222, "Pair", T.ok, kPhPairYes);
      def = 1;
      break;
    }
    case PhView::List: {
      int y = 0;
      if (st.flags & ELC_CAN_PHONE_PAIRING) {
        lv_obj_t *win = box(s_ph_body, 0, y, kPhW, 132);
        lv_obj_set_style_radius(win, 22, 0);
        fill(win, T.surface);
        outline(win, T.accent, 2);
        snprintf(buf, sizeof(buf), "Discoverable as %s", kPhBtName);
        lv_obj_t *l1 = label(win, buf, T.sub, T.text);
        lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 12);
        s_ph_left = label(win, "", T.sub, T.accent);
        lv_obj_align(s_ph_left, LV_ALIGN_TOP_MID, 0, 42);
        lv_obj_t *stop = pill(win, (kPhW - 180) / 2, 76, 180, 46, "Stop", nullptr);
        lv_obj_set_style_text_color(lv_obj_get_child(stop, 0), T.bad, 0);
        ph_add(stop, kPhStop, 0, "Stop pairing");
        y += 132 + kPhGap;
      }
      if (ph_hfp()) {
        const bool voice = (st.flags & ELC_CAN_PHONE_VOICE) != 0;
        ph_row(y, nullptr, T.dim, "Voice assistant", voice ? "On" : nullptr, T.accent, kPhVoice);
        y += kPhRowH + kPhGap;
        ph_row(y, nullptr, T.dim, "Redial", nullptr, T.dim, kPhRedial);
        y += kPhRowH + kPhGap;
      }
      if (!(st.flags & ELC_CAN_PHONE_PAIRING)) {
        ph_row(y, IC_PLUS, T.accent, "Pair new phone", nullptr, T.dim, kPhPairNew);
        y += kPhRowH + kPhGap;
      }
      lv_obj_t *hdr = label(s_ph_body, st.paired ? "PAIRED PHONES" : "NO PAIRED PHONES", T.sub, T.faint);
      lv_obj_set_style_text_letter_space(hdr, 4, 0);
      lv_obj_set_pos(hdr, 12, y + 10);
      y += 48;
      for (uint8_t i = 0; i < st.paired && i < ELC_CAN_PHONE_MAX_PAIRED; i++) {
        const bool on = ph_connected() && st.connected_index == i;
        ph_row(y, IC_BLUETOOTH, on ? T.accent : T.faint, ph_paired(i, nb, sizeof(nb)), on ? "Connected" : nullptr, T.ok,
               kPhPhone, i);
        y += kPhRowH + kPhGap;
      }
      lv_obj_t *pad = box(s_ph_body, 0, y, 1, 8);  // room below the last row when scrolled to the end
      (void)pad;
      break;
    }
    case PhView::Detail: {
      const bool on = ph_connected() && st.connected_index == s_ph_sel;
      ph_text(ph_paired(s_ph_sel, nb, sizeof(nb)), T.body, T.text, 0);
      ph_text(on ? ((st.flags & ELC_CAN_PHONE_HFP) ? "Connected \xC2\xB7 hands-free" : "Connected")
                 : "Paired \xC2\xB7 not connected",
              T.sub, T.dim, 46);
      ph_button(0, 110, kPhW, on ? "Disconnect" : "Connect", T.text, on ? kPhDisconnect : kPhConnect);
      ph_button(0, 110 + 62 + 16, kPhW, "Forget", T.bad, kPhForget);
      break;
    }
    case PhView::Forget:
      snprintf(buf, sizeof(buf), "Forget %s?", ph_paired(s_ph_sel, nb, sizeof(nb)));
      ph_text(buf, T.body, T.text, 0);
      ph_text("It has to pair again before\nit can connect.", T.sub, T.dim, 60, true);
      ph_button(0, 200, 222, "Cancel", T.text, kPhForgetNo);
      ph_button(238, 200, 222, "Forget", T.bad, kPhForgetYes);
      def = 0;
      break;
  }
  if (v == PhView::List) {
    lv_obj_add_flag(s_ph_body, LV_OBJ_FLAG_SCROLLABLE);
  } else {
    lv_obj_clear_flag(s_ph_body, LV_OBJ_FLAG_SCROLLABLE);
  }
  for (int i = 0; i < s_ph_n; i++) ph_style_focus(i, false);
  int f = -1;
  for (int i = 0; i < s_ph_n && f < 0; i++) {
    if (s_ph_items[i].act == s_ph_focus_act && s_ph_items[i].arg == s_ph_focus_arg) f = i;
  }
  if (s_ph_n) {
    s_ph_focus = f >= 0 ? f : (def < s_ph_n ? def : 0);
    ph_set_focus(s_ph_focus, f >= 0);
  }
  s_ph_sig = ph_signature();
  s_ph_dirty = false;
  ph_refresh_status();
}

void build_phone(lv_obj_t *page) {
  page_title(page, "Phone");
  s_ph_sel = 0xFF;
  s_ph_forget = false;
  s_ph_focus_act = 0xFF;
  lv_obj_t *card = box(page, 170, 176, 460, 96);
  lv_obj_set_style_radius(card, 22, 0);
  fill(card, T.surface);
  outline(card, T.line, 2);
  s_ph_st1 = label(card, "", T.body, T.text);
  lv_label_set_long_mode(s_ph_st1, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_ph_st1, 420);
  lv_obj_align(s_ph_st1, LV_ALIGN_TOP_LEFT, 20, 14);
  s_ph_st2 = label(card, "", T.sub, T.dim);
  lv_label_set_long_mode(s_ph_st2, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_ph_st2, 420);
  lv_obj_align(s_ph_st2, LV_ALIGN_TOP_LEFT, 20, 56);
  s_ph_body = box(page, 170, 288, kPhW, 376);
  lv_obj_set_scroll_dir(s_ph_body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_ph_body, LV_SCROLLBAR_MODE_OFF);
  add_back(page);
  ph_build_body();
}

void ph_tick() {
  const uint32_t now = millis();
  if (s_ph_dirty) {
    ph_build_body();
  } else if (now - s_ph_poll_ms >= 200u) {
    s_ph_poll_ms = now;
    if (ph_signature() != s_ph_sig) {
      ph_build_body();
    } else {
      ph_refresh_status();
    }
  }
}

/** The Phone page for the console: the focused choice, and the state (or what the choice is about). */
void ph_describe(char *m, size_t nm, char *sb, size_t ns) {
  const PhView v = ph_view();
  const bool focus = s_ph_n > 0 && s_ph_focus < s_ph_n;
  snprintf(m, nm, "%s", focus ? s_ph_items[s_ph_focus].label : "Phone");
  char nb[16];
  switch (v) {
    case PhView::Detail: snprintf(sb, ns, "%s", ph_paired(s_ph_sel, nb, sizeof(nb))); return;
    case PhView::Forget: snprintf(sb, ns, "Forget %s?", ph_paired(s_ph_sel, nb, sizeof(nb))); return;
    case PhView::List:
      if (focus && s_ph_items[s_ph_focus].act == kPhPhone) {
        const bool on = ph_connected() && ph_st().connected_index == s_ph_items[s_ph_focus].arg;
        snprintf(sb, ns, "%s", on ? "Connected" : "Paired \xC2\xB7 not connected");
        return;
      }
      break;
    default: break;
  }
  ph_summary(sb, ns);
}

// ---------------------------------------------------------------- Accessories
// Every accessory's own page, from the UI channel (accessory_can.h; docs/protocols/can_channels.md, UI): a list of
// accessories, then an accessory's items (groups open sub-pages), and editors for numbers and choices. Toggles and
// actions act at once (after a confirmation when the item asks for one). The body is rebuilt from tick when its
// structure changes; values update in place. The knob moves the focus and presses; in the number editor it turns the
// value. What the screen shows is kept coming with VIEW (accessory_can_view).
enum AcAct : uint8_t { kAcNode, kAcItem, kAcOpt, kAcMinus, kAcPlus, kAcApply, kAcCancel, kAcYes, kAcNo, kAcAdvanced, kAcNone };
enum class AcView : uint8_t { Nodes, Items, Choice, Number, Confirm };

constexpr int kAcMaxRows = 48;
struct AcRow {
  uint8_t act, arg;
  lv_obj_t *obj, *val;
  char label[40];
};
AcRow s_ac_rows[kAcMaxRows];
int s_ac_n = 0, s_ac_focus = 0;
uint8_t s_ac_focus_act = 0xFF, s_ac_focus_arg = 0;
lv_obj_t *s_ac_st1 = nullptr, *s_ac_st2 = nullptr, *s_ac_body = nullptr, *s_ac_big = nullptr;
uint8_t s_ac_node = 0, s_ac_group = ELC_UI_TOP, s_ac_edit = 0xFF;
int32_t s_ac_value = 0;           // the number being edited, or the value waiting for a confirmation
AcView s_ac_back_view = AcView::Items;  // where a confirmation returns to
bool s_ac_adv = false, s_ac_dirty = false;
AcView s_ac_view = AcView::Nodes;
uint32_t s_ac_sig = 0, s_ac_poll_ms = 0;

const AccItem *ac_item(uint8_t item) { return s_ac_node ? accessory_can_item(s_ac_node, item) : nullptr; }

/** The note on an item's row: its value, a pending SET, or the refusal of the last one. */
void ac_note(const AccItem &it, char *out, size_t n, lv_color_t *color) {
  *color = T.dim;
  const uint32_t now = millis();
  if (it.pending) {
    snprintf(out, n, "\xE2\x80\xA6");  // …
    return;
  }
  if (it.result_ms && it.result != 0 && now - it.result_ms < 4000) {
    static const char *const kWhy[5] = {"", "Out of range", "Read-only", "Busy", "Unknown"};
    snprintf(out, n, "%s", it.result == 0xFF ? "No answer" : (it.result < 5 ? kWhy[it.result] : "Refused"));
    *color = T.bad;
    return;
  }
  accessory_can_value_text(it, out, n);
  if (it.d.kind == ELC_UI_TOGGLE && it.has_value && it.value) *color = T.accent;
}

uint32_t ac_signature() {
  uint32_t h = fnv(2166136261u, &s_ac_view, sizeof(s_ac_view));
  const uint8_t k[5] = {s_ac_node, s_ac_group, s_ac_edit, (uint8_t)s_ac_adv, (uint8_t)(s_ac_value & 0xFF)};
  h = fnv(h, k, sizeof(k));
  if (s_ac_view == AcView::Nodes) {
    uint8_t nodes[kAccMaxNodes];
    const int n = accessory_can_nodes(nodes, kAccMaxNodes);
    h = fnv(h, nodes, (size_t)n);
    for (int i = 0; i < n; i++) h = fnv_str(h, accessory_can_name(nodes[i]));
    return h;
  }
  // the items' descriptions (not their values, which update in place)
  const AccItem *items[kAcMaxRows];
  const int n = accessory_can_items(s_ac_node, s_ac_group, s_ac_adv, items, kAcMaxRows);
  for (int i = 0; i < n; i++) {
    h = fnv(h, &items[i]->d.item, 1);
    h = fnv_str(h, items[i]->d.label);
    h = fnv_str(h, items[i]->d.options);
  }
  if (s_ac_view == AcView::Choice || s_ac_view == AcView::Number) {
    const AccItem *it = ac_item(s_ac_edit);
    if (it) h = fnv(h, &it->d, sizeof(it->d.kind));
  }
  return h;
}

void ac_style_focus(int i, bool on) {
  if (i < 0 || i >= s_ac_n || !s_ac_rows[i].obj) return;
  lv_obj_set_style_border_color(s_ac_rows[i].obj, on ? T.accent : T.line, 0);
  lv_obj_set_style_border_width(s_ac_rows[i].obj, on ? 3 : 2, 0);
}

void ac_set_focus(int i, bool scroll) {
  if (s_ac_n == 0) return;
  if (i < 0) i = 0;
  if (i >= s_ac_n) i = s_ac_n - 1;
  if (i != s_ac_focus) ac_style_focus(s_ac_focus, false);
  s_ac_focus = i;
  ac_style_focus(i, true);
  s_ac_focus_act = s_ac_rows[i].act;
  s_ac_focus_arg = s_ac_rows[i].arg;
  if (scroll) lv_obj_scroll_to_view(s_ac_rows[i].obj, LV_ANIM_ON);
}

/** Send a SET, or ask first when the item wants a confirmation. */
void ac_send(const AccItem &it, int32_t value, AcView from) {
  if (it.d.flags & ELC_UI_F_CONFIRM) {
    s_ac_edit = it.d.item;
    s_ac_value = value;
    s_ac_back_view = from;
    s_ac_view = AcView::Confirm;
    s_ac_focus_act = kAcNo;
    return;
  }
  accessory_can_set(s_ac_node, it.d.item, value);
  s_ac_view = AcView::Items;
  s_ac_focus_act = kAcItem;
  s_ac_focus_arg = it.d.item;
}

int32_t ac_clamp(const AccItem &it, int32_t v) {
  if (it.d.max > it.d.min) {
    if (v < it.d.min) v = it.d.min;
    if (v > it.d.max) v = it.d.max;
  }
  return v;
}

void ac_step(int steps) {
  const AccItem *it = ac_item(s_ac_edit);
  if (!it) return;
  const int32_t step = it->d.step > 0 ? it->d.step : 1;
  s_ac_value = ac_clamp(*it, s_ac_value + steps * step);
  if (s_ac_big) {
    char v[24], b[40];
    elc_ui_format(s_ac_value, it->d.exponent, v, sizeof(v));
    snprintf(b, sizeof(b), it->d.unit[0] ? "%s %s" : "%s", v, it->d.unit);
    lv_label_set_text(s_ac_big, b);
  }
}

void ac_activate(int i) {
  if (i < 0 || i >= s_ac_n) return;
  const AcRow row = s_ac_rows[i];
  switch (row.act) {
    case kAcNode:
      s_ac_node = row.arg;
      s_ac_group = ELC_UI_TOP;
      s_ac_adv = false;
      s_ac_view = AcView::Items;
      s_ac_focus_act = 0xFF;
      break;
    case kAcAdvanced:
      s_ac_adv = !s_ac_adv;
      break;
    case kAcItem: {
      const AccItem *it = ac_item(row.arg);
      if (!it || (it->d.flags & ELC_UI_F_RO)) break;
      switch (it->d.kind) {
        case ELC_UI_GROUP:
          s_ac_group = it->d.item;
          s_ac_focus_act = 0xFF;
          break;
        case ELC_UI_TOGGLE: ac_send(*it, it->has_value && it->value ? 0 : 1, AcView::Items); break;
        case ELC_UI_ACTION: ac_send(*it, 1, AcView::Items); break;
        case ELC_UI_CHOICE:
          s_ac_edit = it->d.item;
          s_ac_view = AcView::Choice;
          s_ac_focus_act = kAcOpt;
          s_ac_focus_arg = (uint8_t)(it->has_value ? it->value : 0);
          break;
        case ELC_UI_NUMBER:
          s_ac_edit = it->d.item;
          s_ac_value = ac_clamp(*it, it->has_value ? it->value : it->d.min);
          s_ac_view = AcView::Number;
          s_ac_focus_act = kAcApply;
          break;
        default: break;  // readouts
      }
      break;
    }
    case kAcOpt: {
      const AccItem *it = ac_item(s_ac_edit);
      if (it) ac_send(*it, row.arg, AcView::Choice);
      break;
    }
    case kAcMinus: ac_step(-1); return;
    case kAcPlus: ac_step(1); return;
    case kAcApply: {
      const AccItem *it = ac_item(s_ac_edit);
      if (it) ac_send(*it, s_ac_value, AcView::Number);
      break;
    }
    case kAcCancel:
      s_ac_view = AcView::Items;
      s_ac_focus_act = kAcItem;
      s_ac_focus_arg = s_ac_edit;
      break;
    case kAcYes: {
      accessory_can_set(s_ac_node, s_ac_edit, s_ac_value);
      s_ac_view = AcView::Items;
      s_ac_focus_act = kAcItem;
      s_ac_focus_arg = s_ac_edit;
      break;
    }
    case kAcNo:
      s_ac_view = s_ac_back_view == AcView::Confirm ? AcView::Items : s_ac_back_view;
      s_ac_focus_act = s_ac_view == AcView::Items ? kAcItem : 0xFF;
      s_ac_focus_arg = s_ac_edit;
      break;
    default: return;
  }
  s_ac_dirty = true;  // tick rebuilds (this may run inside the clicked object's handler)
}

void ac_clicked(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_ac_n) return;
  ac_set_focus(i, false);
  ac_activate(i);
}

lv_obj_t *ac_add(lv_obj_t *o, lv_obj_t *val, uint8_t act, uint8_t arg, const char *text) {
  if (s_ac_n >= kAcMaxRows) return o;
  AcRow &r = s_ac_rows[s_ac_n];
  r.act = act;
  r.arg = arg;
  r.obj = o;
  r.val = val;
  snprintf(r.label, sizeof(r.label), "%s", text);
  lv_obj_add_event_cb(o, ac_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)s_ac_n);
  s_ac_n++;
  return o;
}

/** A list row: text on the left, a note (the value) on the right. */
void ac_row(int y, const char *text, const char *note, lv_color_t note_color, uint8_t act, uint8_t arg, bool dim = false) {
  lv_obj_t *row = box(s_ac_body, 0, y, kPhW, kPhRowH);
  lv_obj_set_style_radius(row, 18, 0);
  fill(row, T.surface);
  outline(row, T.line, 2);
  lv_obj_set_style_bg_color(row, T.line, LV_STATE_PRESSED);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *l = label(row, text, T.sub, dim ? T.dim : T.text);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_width(l, note && note[0] ? 220 : kPhW - 44);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 22, 0);
  lv_obj_t *nl = nullptr;
  if (note) {
    nl = label(row, note, T.sub, note_color);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nl, kPhW - 44 - 230);
    lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(nl, LV_ALIGN_RIGHT_MID, -20, 0);
  }
  ac_add(row, nl, act, arg, text);
}

lv_obj_t *ac_text(const char *text, const lv_font_t *font, lv_color_t color, int y, bool wrap = false) {
  lv_obj_t *l = label(s_ac_body, text, font, color);
  lv_obj_set_width(l, kPhW);
  lv_label_set_long_mode(l, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(l, 0, y);
  return l;
}

lv_obj_t *ac_button(int x, int y, int w, const char *text, lv_color_t color, uint8_t act, uint8_t arg = 0) {
  lv_obj_t *b = pill(s_ac_body, x, y, w, 62, text, nullptr);
  lv_obj_set_style_text_color(lv_obj_get_child(b, 0), color, 0);
  return ac_add(b, nullptr, act, arg, text);
}

void ac_status() {
  if (!s_ac_st1) return;
  char l1[64], l2[80];
  if (s_ac_view == AcView::Nodes) {
    uint8_t nodes[kAccMaxNodes];
    const int n = accessory_can_nodes(nodes, kAccMaxNodes);
    snprintf(l1, sizeof(l1), "Accessories");
    if (n) {
      snprintf(l2, sizeof(l2), "%d on the bus", n);
    } else {
      snprintf(l2, sizeof(l2), "None has sent its page yet");
    }
  } else {
    snprintf(l1, sizeof(l1), "%s", accessory_can_name(s_ac_node));
    const AccItem *g = s_ac_group != ELC_UI_TOP ? ac_item(s_ac_group) : nullptr;
    const ElcChannels *ch = chan_screen();
    const bool alive = ch && ch->nodeInfo(s_ac_node).announced;
    if (g) {
      snprintf(l2, sizeof(l2), "%s", g->d.label);
    } else {
      snprintf(l2, sizeof(l2), "Node 0x%02X%s", (unsigned)s_ac_node, alive ? "" : " \xC2\xB7 not announced");
    }
  }
  if (strcmp(lv_label_get_text(s_ac_st1), l1) != 0) lv_label_set_text(s_ac_st1, l1);
  if (strcmp(lv_label_get_text(s_ac_st2), l2) != 0) lv_label_set_text(s_ac_st2, l2);
}

/** Values in place: each item row's note. */
void ac_refresh_values() {
  if (s_ac_view != AcView::Items) return;
  for (int i = 0; i < s_ac_n; i++) {
    const AcRow &r = s_ac_rows[i];
    if (r.act != kAcItem || !r.val) continue;
    const AccItem *it = ac_item(r.arg);
    if (!it) continue;
    char note[72];
    lv_color_t c;
    ac_note(*it, note, sizeof(note), &c);
    if (strcmp(lv_label_get_text(r.val), note) != 0) lv_label_set_text(r.val, note);
    lv_obj_set_style_text_color(r.val, c, 0);
  }
}

void ac_build_body() {
  if (!s_ac_body) return;
  lv_obj_clean(s_ac_body);
  lv_obj_scroll_to_y(s_ac_body, 0, LV_ANIM_OFF);
  s_ac_n = 0;
  s_ac_focus = 0;
  s_ac_big = nullptr;
  const AccItem *edit = ac_item(s_ac_edit);
  if ((s_ac_view == AcView::Choice || s_ac_view == AcView::Number || s_ac_view == AcView::Confirm) && !edit) {
    s_ac_view = AcView::Items;  // the item went away
  }
  char buf[96];
  int y = 0, def = 0;
  switch (s_ac_view) {
    case AcView::Nodes: {
      uint8_t nodes[kAccMaxNodes];
      const int n = accessory_can_nodes(nodes, kAccMaxNodes);
      if (!n) {
        ac_text("Accessory cards put their settings\nand readouts here once they are\non the bus.", T.sub, T.faint, 60, true);
      }
      for (int i = 0; i < n; i++) {
        ac_row(y, accessory_can_name(nodes[i]), ">", T.dim, kAcNode, nodes[i]);
        y += kPhRowH + kPhGap;
      }
      break;
    }
    case AcView::Items: {
      const AccItem *items[kAcMaxRows];
      const int n = accessory_can_items(s_ac_node, s_ac_group, s_ac_adv, items, kAcMaxRows - 1);
      if (!n) ac_text("Nothing on this page yet.", T.sub, T.faint, 60);
      for (int i = 0; i < n; i++) {
        char note[72];
        lv_color_t c;
        ac_note(*items[i], note, sizeof(note), &c);
        const bool ro = (items[i]->d.flags & ELC_UI_F_RO) || items[i]->d.kind == ELC_UI_READOUT ||
                        items[i]->d.kind == ELC_UI_TEXTOUT;
        ac_row(y, items[i]->d.label, note, c, kAcItem, items[i]->d.item, ro);
        y += kPhRowH + kPhGap;
      }
      // advanced items: a switch at the end of the accessory's top page, when it has any
      const AccItem *all[kAcMaxRows];
      const int na = s_ac_group == ELC_UI_TOP ? accessory_can_items(s_ac_node, s_ac_group, true, all, kAcMaxRows) : 0;
      if (na > n || s_ac_adv) {
        ac_row(y, "Show advanced", s_ac_adv ? "On" : "Off", s_ac_adv ? T.accent : T.dim, kAcAdvanced, 0);
        y += kPhRowH + kPhGap;
      }
      box(s_ac_body, 0, y, 1, 8);
      break;
    }
    case AcView::Choice: {
      ac_text(edit->d.label, T.body, T.text, 0);
      y = 56;
      const int n = edit->d.option_count();
      for (int k = 0; k < n && k < 255; k++) {
        char opt[48];
        if (!edit->d.option(k, opt, sizeof(opt))) break;
        const bool cur = edit->has_value && edit->value == k;
        ac_row(y, opt, cur ? "Current" : nullptr, T.accent, kAcOpt, (uint8_t)k);
        y += kPhRowH + kPhGap;
      }
      box(s_ac_body, 0, y, 1, 8);
      break;
    }
    case AcView::Number: {
      ac_text(edit->d.label, T.body, T.text, 0);
      s_ac_big = ac_text("", T.label, T.accent, 70);
      ac_step(0);
      char lo[24], hi[24];
      elc_ui_format(edit->d.min, edit->d.exponent, lo, sizeof(lo));
      elc_ui_format(edit->d.max, edit->d.exponent, hi, sizeof(hi));
      snprintf(buf, sizeof(buf), "%s to %s %s", lo, hi, edit->d.unit);
      ac_text(buf, T.sub, T.faint, 140);
      ac_button(0, 190, 222, "-", T.text, kAcMinus);
      ac_button(238, 190, 222, "+", T.text, kAcPlus);
      ac_button(0, 268, 222, "Cancel", T.dim, kAcCancel);
      ac_button(238, 268, 222, "Set", T.ok, kAcApply);
      def = 3;
      break;
    }
    case AcView::Confirm: {
      char v[48] = "";
      if (edit->d.kind == ELC_UI_CHOICE) {
        edit->d.option((int)s_ac_value, v, sizeof(v));
      } else if (edit->d.kind == ELC_UI_NUMBER) {
        elc_ui_format(s_ac_value, edit->d.exponent, v, sizeof(v));
      }
      if (v[0]) {
        snprintf(buf, sizeof(buf), "%s: %s?", edit->d.label, v);
      } else {
        snprintf(buf, sizeof(buf), "%s?", edit->d.label);
      }
      ac_text(buf, T.body, T.text, 30, true);
      ac_button(0, 200, 222, "Cancel", T.text, kAcNo);
      ac_button(238, 200, 222, "OK", T.ok, kAcYes);
      def = 0;
      break;
    }
  }
  const bool list = s_ac_view == AcView::Nodes || s_ac_view == AcView::Items || s_ac_view == AcView::Choice;
  if (list) {
    lv_obj_add_flag(s_ac_body, LV_OBJ_FLAG_SCROLLABLE);
  } else {
    lv_obj_clear_flag(s_ac_body, LV_OBJ_FLAG_SCROLLABLE);
  }
  for (int i = 0; i < s_ac_n; i++) ac_style_focus(i, false);
  int f = -1;
  for (int i = 0; i < s_ac_n && f < 0; i++) {
    if (s_ac_rows[i].act == s_ac_focus_act && s_ac_rows[i].arg == s_ac_focus_arg) f = i;
  }
  if (s_ac_n) {
    s_ac_focus = f >= 0 ? f : (def < s_ac_n ? def : 0);
    ac_set_focus(s_ac_focus, f >= 0);
  }
  s_ac_sig = ac_signature();
  s_ac_dirty = false;
  ac_status();
}

void build_accessories(lv_obj_t *page) {
  page_title(page, "Accessories");
  s_ac_view = AcView::Nodes;
  s_ac_node = 0;
  s_ac_group = ELC_UI_TOP;
  s_ac_edit = 0xFF;
  s_ac_focus_act = 0xFF;
  lv_obj_t *card = box(page, 170, 176, 460, 96);
  lv_obj_set_style_radius(card, 22, 0);
  fill(card, T.surface);
  outline(card, T.line, 2);
  s_ac_st1 = label(card, "", T.body, T.text);
  lv_label_set_long_mode(s_ac_st1, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_ac_st1, 420);
  lv_obj_align(s_ac_st1, LV_ALIGN_TOP_LEFT, 20, 14);
  s_ac_st2 = label(card, "", T.sub, T.dim);
  lv_label_set_long_mode(s_ac_st2, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_ac_st2, 420);
  lv_obj_align(s_ac_st2, LV_ALIGN_TOP_LEFT, 20, 56);
  s_ac_body = box(page, 170, 288, kPhW, 376);
  lv_obj_set_scroll_dir(s_ac_body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_ac_body, LV_SCROLLBAR_MODE_OFF);
  add_back(page);
  ac_build_body();
}

void ac_tick() {
  // keep the shown accessory's live items coming
  accessory_can_view(s_ac_view == AcView::Nodes ? 0 : s_ac_node, s_ac_group);
  const uint32_t now = millis();
  if (s_ac_dirty) {
    ac_build_body();
  } else if (now - s_ac_poll_ms >= 200u) {
    s_ac_poll_ms = now;
    if (ac_signature() != s_ac_sig) {
      ac_build_body();
    } else {
      ac_refresh_values();
      ac_status();
    }
  }
}

/** Back one step inside the page; false when already at the list of accessories. */
bool ac_back() {
  switch (s_ac_view) {
    case AcView::Confirm:
      s_ac_view = s_ac_back_view == AcView::Confirm ? AcView::Items : s_ac_back_view;
      break;
    case AcView::Choice:
    case AcView::Number:
      s_ac_view = AcView::Items;
      s_ac_focus_act = kAcItem;
      s_ac_focus_arg = s_ac_edit;
      break;
    case AcView::Items:
      if (s_ac_group != ELC_UI_TOP) {
        const AccItem *g = ac_item(s_ac_group);
        s_ac_focus_act = kAcItem;
        s_ac_focus_arg = s_ac_group;
        s_ac_group = g ? g->d.parent : ELC_UI_TOP;
      } else {
        s_ac_view = AcView::Nodes;
        s_ac_focus_act = kAcNode;
        s_ac_focus_arg = s_ac_node;
        s_ac_node = 0;
      }
      break;
    case AcView::Nodes: return false;
  }
  s_ac_dirty = true;
  return true;
}

/** The Accessories page for the console: the focused row, and its value (or the editor's value). */
void ac_describe(char *m, size_t nm, char *sb, size_t ns) {
  const bool focus = s_ac_n > 0 && s_ac_focus < s_ac_n;
  const AccItem *edit = ac_item(s_ac_edit);
  if (s_ac_view == AcView::Number && edit) {
    char v[24];
    elc_ui_format(s_ac_value, edit->d.exponent, v, sizeof(v));
    snprintf(m, nm, "%s %s %s", edit->d.label, v, edit->d.unit);
    snprintf(sb, ns, "Turn to change \xC2\xB7 Set");
    return;
  }
  snprintf(m, nm, "%s", focus ? s_ac_rows[s_ac_focus].label : "Accessories");
  if (focus && s_ac_rows[s_ac_focus].act == kAcItem) {
    const AccItem *it = ac_item(s_ac_rows[s_ac_focus].arg);
    lv_color_t c;
    if (it) ac_note(*it, sb, ns, &c);
    return;
  }
  if (s_ac_view == AcView::Nodes) {
    snprintf(sb, ns, "%s", focus ? "Open its page" : "No accessory pages");
  } else {
    snprintf(sb, ns, "%s", accessory_can_name(s_ac_node));
  }
}

const char *ac_home_sub() {
  static char buf[48];
  uint8_t nodes[kAccMaxNodes];
  const int n = accessory_can_nodes(nodes, kAccMaxNodes);
  if (!n) return "None on the bus";
  if (n == 1) return accessory_can_name(nodes[0]);
  snprintf(buf, sizeof(buf), "%d on the bus", n);
  return buf;
}

// ---------------------------------------------------------------- lifecycle
void clear_page_refs() {
  s_bright_arc = s_bright_val = s_follow_sw = nullptr;
  memset(s_svc_val, 0, sizeof(s_svc_val));
  memset(s_svc_dot, 0, sizeof(s_svc_dot));
  memset(s_screen_btn, 0, sizeof(s_screen_btn));
  s_demo_btn_lbl = nullptr;
  s_ph_st1 = s_ph_st2 = s_ph_body = s_ph_left = nullptr;
  s_ph_n = 0;
  s_ac_st1 = s_ac_st2 = s_ac_body = s_ac_big = nullptr;
  s_ac_n = 0;
}

void tick(lv_timer_t *) {
  if (!s_root) {
    return;
  }
  if (host_theme() != s_theme_built) {
    ui_menu_open(s_page_id);  // theme changed (here or from another screen): rebuild in the new one
    return;
  }
  static uint32_t s_last = 0;
  const bool slow = millis() - s_last > 500;
  if (slow) s_last = millis();
  if (s_page_id == UiMenuPage::Home) {
    if (!s_dragging && fabsf(s_pos - (float)s_target) > 0.002f) {
      s_pos += ((float)s_target - s_pos) * 0.28f;
      if (fabsf(s_pos - (float)s_target) < 0.002f) {
        s_pos = (float)s_target;
      }
      layout_wheel();
    }
  } else if (s_page_id == UiMenuPage::Service) {
    if (slow) svc_refresh();
  } else if (s_page_id == UiMenuPage::Screens) {
    if (screens_sig() != s_screens_sig) screens_restyle();
  } else if (s_page_id == UiMenuPage::Phone) {
    ph_tick();
  } else if (s_page_id == UiMenuPage::Accessories) {
    ac_tick();
  }
  if (s_page_id != UiMenuPage::Accessories) accessory_can_view(0, ELC_UI_TOP);
}

}  // namespace

const char *ui_screen_name(uint8_t screen) {
  return screen == kUiScreenLeft ? "Left" : (screen == kUiScreenRight ? "Right" : "Center");
}

void ui_menu_begin(const UiMenuHost *host) {
  H = host;
  if (H->brightness) s_level = H->brightness();
  if (H->dim_with_lights) s_follow = H->dim_with_lights();

  // Hold-progress ring for gauge-screen gestures (lives outside the menu).
  s_hold_arc = lv_arc_create(p4_screen_ui_parent());
  lv_obj_set_size(s_hold_arc, kW - 24, kH - 24);
  lv_obj_center(s_hold_arc);
  lv_arc_set_rotation(s_hold_arc, 270);
  lv_arc_set_bg_angles(s_hold_arc, 0, 360);
  lv_arc_set_range(s_hold_arc, 0, 1000);
  lv_obj_remove_style(s_hold_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(s_hold_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_hold_arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(s_hold_arc, LV_OPA_50, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_hold_arc, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_hold_arc, 12, LV_PART_INDICATOR);
  lv_obj_add_flag(s_hold_arc, LV_OBJ_FLAG_HIDDEN);
  s_hold_lbl = lv_label_create(p4_screen_ui_parent());
  lv_obj_remove_style_all(s_hold_lbl);
  lv_obj_set_style_bg_opa(s_hold_lbl, LV_OPA_80, 0);
  lv_obj_set_style_bg_color(s_hold_lbl, lv_color_black(), 0);
  lv_obj_set_style_radius(s_hold_lbl, 20, 0);
  lv_obj_set_style_pad_hor(s_hold_lbl, 18, 0);
  lv_obj_set_style_pad_ver(s_hold_lbl, 6, 0);
  lv_obj_align(s_hold_lbl, LV_ALIGN_BOTTOM_MID, 0, -52);
  lv_obj_add_flag(s_hold_lbl, LV_OBJ_FLAG_HIDDEN);
}

void ui_menu_open(UiMenuPage page) {
  if (!H || page == UiMenuPage::Reserved1 || (uint8_t)page >= kUiMenuPages) {
    return;
  }
  const bool was_open = s_root != nullptr;
  if (was_open && host_theme() != s_theme_built) {
    ui_menu_close();  // theme changed while open: rebuild everything
    ui_menu_open(page);
    return;
  }
  if (!s_root) {
    s_theme_built = host_theme();
    T = theme_for(s_theme_built);
    if (H->brightness) s_level = H->brightness();
    if (H->dim_with_lights) s_follow = H->dim_with_lights();
    p4_screen_set_tear_free(false);  // menu motion is big; favour frame rate over tear-free flips
    s_root = box(p4_screen_ui_parent(), 0, 0, kW, kH);
    // Opaque square backdrop: LVGL skips the frozen gauge layer underneath, and a square (not a
    // circle) avoids an anti-aliased radius mask on every repainted row. The panel is round anyway.
    fill(s_root, T.bg);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);  // swallow taps outside controls
    s_timer = lv_timer_create(tick, 16, nullptr);
  }
  if (s_page) {
    lv_obj_del_async(s_page);  // may be called from a click inside the old page
  }
  clear_page_refs();
  s_page = box(s_root, 0, 0, kW, kH);
  s_page_id = page;
  switch (page) {
    case UiMenuPage::Home: {
      int cur = 0;
      for (int i = 0; i < kNumItems; i++) {
        if (kItems[i].act == Act::View && kItems[i].arg == center_view()) cur = i;
      }
      if (!was_open) {
        s_target = cur;
        s_pos = (float)cur;
      }
      build_home(s_page);
      break;
    }
    case UiMenuPage::Reserved1: break;
    case UiMenuPage::Brightness: build_brightness(s_page); break;
    case UiMenuPage::Theme: build_theme(s_page); break;
    case UiMenuPage::Screens: build_screens(s_page); break;
    case UiMenuPage::Service: build_service(s_page); break;
    case UiMenuPage::Phone: build_phone(s_page); break;
    case UiMenuPage::Accessories: build_accessories(s_page); break;
  }
  fade_in(was_open ? s_page : s_root, was_open ? 200 : 220);
}

void ui_menu_close() {
  if (s_timer) {
    lv_timer_del(s_timer);
    s_timer = nullptr;
  }
  if (s_root) {
    lv_obj_del_async(s_root);  // usually called from a click handler inside the menu
  }
  s_root = nullptr;
  s_page = nullptr;
  clear_page_refs();
  accessory_can_view(0, ELC_UI_TOP);  // no accessory page shows now
  p4_screen_set_tear_free(true);
  s_dragging = false;
}

bool ui_menu_is_open() { return s_root != nullptr; }

UiMenuPage ui_menu_page() { return s_page_id; }

void ui_menu_rotate(int steps) {
  if (!s_root) {
    return;
  }
  if (s_page_id == UiMenuPage::Home) {
    s_target = s_target + steps;
    if (s_target < 0) s_target = 0;
    if (s_target >= kNumItems) s_target = kNumItems - 1;
  } else if (s_page_id == UiMenuPage::Brightness) {
    int v = (int)s_level + steps * 5;
    s_level = (uint8_t)(v < 10 ? 10 : (v > 100 ? 100 : v));
    bright_apply(true);
  } else if (s_page_id == UiMenuPage::Phone) {
    ph_set_focus(s_ph_focus + steps, true);
  } else if (s_page_id == UiMenuPage::Accessories) {
    if (s_ac_view == AcView::Number) {
      ac_step(steps);
    } else {
      ac_set_focus(s_ac_focus + steps, true);
    }
  }
}

void ui_menu_press() {
  if (!s_root) {
    ui_menu_open(UiMenuPage::Home);
  } else if (s_page_id == UiMenuPage::Home) {
    activate(s_target);
  } else if (s_page_id == UiMenuPage::Phone) {
    if (s_ph_n && !s_ph_dirty) ph_activate(s_ph_focus);  // not twice before the page shows the outcome
  } else if (s_page_id == UiMenuPage::Accessories) {
    if (s_ac_view == AcView::Number && !s_ac_dirty) {
      s_ac_focus_act = kAcApply;  // press sets the number, whatever has the focus
      for (int i = 0; i < s_ac_n; i++) {
        if (s_ac_rows[i].act == kAcApply) ac_activate(i);
      }
    } else if (s_ac_n && !s_ac_dirty) {
      ac_activate(s_ac_focus);
    }
  } else {
    ui_menu_open(UiMenuPage::Home);
  }
}

void ui_menu_back() {
  if (!s_root) {
    return;
  }
  if (s_page_id == UiMenuPage::Phone && ph_view() == PhView::Forget) {
    s_ph_forget = false;
    s_ph_focus_act = kPhForget;
    s_ph_dirty = true;
  } else if (s_page_id == UiMenuPage::Phone && ph_view() == PhView::Detail) {
    s_ph_focus_act = kPhPhone;
    s_ph_focus_arg = s_ph_sel;
    s_ph_sel = 0xFF;
    s_ph_dirty = true;
  } else if (s_page_id == UiMenuPage::Accessories && ac_back()) {
    // a step back inside the page
  } else if (s_page_id != UiMenuPage::Home) {
    // Home with the wheel on the page just left.
    for (int i = 0; i < kNumItems; i++) {
      if (kItems[i].act == Act::Page && kItems[i].arg == (uint8_t)s_page_id) {
        s_target = i;
        s_pos = (float)i;
      }
    }
    ui_menu_open(UiMenuPage::Home);
  } else {
    ui_menu_close();
  }
}

void ui_menu_describe(char *m, size_t nm, char *sb, size_t ns) {
  if (!m || !sb || !nm || !ns) {
    return;
  }
  m[0] = sb[0] = '\0';
  if (!s_root) {
    return;
  }
  switch (s_page_id) {
    case UiMenuPage::Home:
      snprintf(m, nm, "%s", kItems[s_target].label);
      snprintf(sb, ns, "%s", item_sub(s_target));
      break;
    case UiMenuPage::Brightness:
      snprintf(m, nm, "Brightness %u%%", (unsigned)s_level);
      snprintf(sb, ns, "%s", s_follow ? "Dims with headlights" : "Fixed level");
      break;
    case UiMenuPage::Theme: {
      const uint8_t th = host_theme();
      snprintf(m, nm, "Theme %s", th == 1 ? "Fallout" : (th == 2 ? "Modern" : "Classic"));
      snprintf(sb, ns, "All three screens");
      break;
    }
    case UiMenuPage::Screens:
      snprintf(m, nm, "Menu %s \xC2\xB7 Media %s",
               ui_screen_name(H->interface_screen ? H->interface_screen() : kUiScreenCenter), ui_screen_name(media_screen()));
      snprintf(sb, ns, "Choose on the screen");
      break;
    case UiMenuPage::Service: {
      snprintf(m, nm, "Service");
      snprintf(sb, ns, "No faults");
      for (int i = 0; i < kUiServiceRows && H->service_row; i++) {
        char v[48] = "";
        int level = 1;
        H->service_row(i, v, sizeof(v), &level);
        if (level == 2) {
          snprintf(sb, ns, "%s: %s", kSvcNames[i], v);
          break;
        }
      }
      break;
    }
    case UiMenuPage::Phone: ph_describe(m, nm, sb, ns); break;
    case UiMenuPage::Accessories: ac_describe(m, nm, sb, ns); break;
    case UiMenuPage::Reserved1: break;
  }
  utf8_cut(m, 32);
  utf8_cut(sb, 32);
}

void ui_menu_hold_feedback(bool active, int32_t progress, const char *message) {
  if (!s_hold_arc) {
    return;
  }
  if (!active || s_root) {
    if (!lv_obj_has_flag(s_hold_arc, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_add_flag(s_hold_arc, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_hold_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }
  // Setting a style repaints the whole 776 px ring, so only restyle when the theme changes; the
  // value and text are set only when they change (each repaints just what moved).
  static int s_styled = -1;
  const uint8_t th = host_theme();
  if (s_styled != (int)th) {
    s_styled = (int)th;
    const Theme t = theme_for(th);
    lv_obj_set_style_arc_color(s_hold_arc, t.accent, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(s_hold_lbl, t.sub, 0);
    lv_obj_set_style_text_color(s_hold_lbl, t.accent, 0);
  }
  const int32_t v = progress < 0 ? 0 : (progress > 1000 ? 1000 : progress);
  if (lv_arc_get_value(s_hold_arc) != v) {
    lv_arc_set_value(s_hold_arc, v);
  }
  if (message && strcmp(lv_label_get_text(s_hold_lbl), message) != 0) {
    lv_label_set_text(s_hold_lbl, message);
  }
  if (lv_obj_has_flag(s_hold_arc, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_clear_flag(s_hold_arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hold_lbl, LV_OBJ_FLAG_HIDDEN);
  }
}
