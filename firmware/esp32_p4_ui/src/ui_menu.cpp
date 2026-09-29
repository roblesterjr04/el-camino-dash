#include "ui_menu.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <elc_fonts.h>
#include <p4_screen.h>

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
#define IC_WIFI "\xEE\x98\xBE"
#define IC_BRIGHT "\xEE\x8E\xAB"
#define IC_PALETTE "\xEE\x90\x8A"
#define IC_SETTINGS "\xEE\xA2\xB8"
#define IC_BUILD "\xEE\xA1\xA9"
#define IC_BACK "\xEE\x97\x84"
#define IC_CLOSE "\xEE\x97\x8D"
#define IC_LOCK "\xEE\xA2\x97"
#define IC_MINUS "\xEE\x85\x9B"
#define IC_PLUS "\xEE\x85\x85"

namespace {

constexpr int kW = 800;
constexpr int kH = 800;
constexpr int kItemGap = 130;   // wheel spacing
constexpr float kCurveR = 490;  // wheel curvature (px)

const UiMenuHost *H = nullptr;

struct Theme {
  lv_color_t bg, text, dim, faint, accent, ink, surface, line, ok, bad;
  const lv_font_t *label, *title, *body, *sub;
};

// Theme numbering as on the cluster bus: 0 Classic, 1 Fallout, 2 Modern.
Theme theme_for(uint8_t t) {
  Theme th{};
  if (t == 1) {
    th = {lv_color_hex(0x010a04), lv_color_hex(0x5dff8f), lv_color_hex(0x2fae5c), lv_color_hex(0x17602f),
          lv_color_hex(0xb8ffcc), lv_color_hex(0x021008), lv_color_hex(0x06200f), lv_color_hex(0x1a6a35),
          lv_color_hex(0xb8ffcc), lv_color_hex(0xffb347), &elc_vt_72, &elc_vt_56, &elc_vt_36, &elc_vt_28};
  } else if (t == 2) {
    th = {lv_color_hex(0x060708), lv_color_hex(0xf3f5f6), lv_color_hex(0x9aa4aa), lv_color_hex(0x5d666c),
          lv_color_hex(0xff7a2f), lv_color_hex(0x0b0c0d), lv_color_hex(0x171b1e), lv_color_hex(0x2b3136),
          lv_color_hex(0x45d483), lv_color_hex(0xff3b30), &elc_sa_56, &elc_sa_46, &elc_sm_28, &elc_sm_22};
  } else {
    th = {lv_color_hex(0x0c0b09), lv_color_hex(0xefe7d5), lv_color_hex(0xa79d88), lv_color_hex(0x6f6758),
          lv_color_hex(0xe8a33d), lv_color_hex(0x1e1405), lv_color_hex(0x1d1a15), lv_color_hex(0x3d372e),
          lv_color_hex(0x7fcf8a), lv_color_hex(0xff6a55), &elc_bc_58, &elc_bc_46, &elc_b_28, &elc_b_22};
  }
  return th;
}

uint8_t host_theme() { return H && H->theme ? H->theme() : 0; }

enum class Act : uint8_t { View, Page, Phone };
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
    {IC_PHONE, "Phone", Act::Phone, 0},
    {IC_WIFI, "Wi-Fi", Act::Page, (uint8_t)UiMenuPage::Wifi},
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
lv_obj_t *s_wifi_status = nullptr, *s_wifi_status2 = nullptr, *s_wifi_list = nullptr, *s_wifi_scan_lbl = nullptr;
bool s_scanning = false;
UiMenuWifiNet s_scan[8];
int s_scan_n = 0;
lv_obj_t *s_bright_arc = nullptr, *s_bright_val = nullptr, *s_follow_sw = nullptr;
uint8_t s_level = 100;
bool s_follow = true;
lv_obj_t *s_svc_val[8];
lv_obj_t *s_svc_dot[8];
lv_obj_t *s_demo_btn_lbl = nullptr;
lv_obj_t *s_screen_btn[2][3];  // [menu / media][left, center, right]
int s_screens_sig = -1;

// hold ring on the gauge screen
lv_obj_t *s_hold_arc = nullptr, *s_hold_lbl = nullptr;

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

void back_home(lv_event_t *) { ui_menu_open(UiMenuPage::Home); }

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
      case kUiViewClock: return "Time and street";
      case kUiViewEcon: return "MPG and range";
      default:
        if (media_screen() != kUiScreenCenter) {
          snprintf(buf, sizeof(buf), "On the %s screen", media_screen() == kUiScreenLeft ? "left" : "right");
          return buf;
        }
        return "Bluetooth audio";
    }
  }
  if (it.act == Act::Phone) {
    return H->self_screen == kUiScreenCenter ? "Bluetooth link" : "Opens on the center";
  }
  switch ((UiMenuPage)it.arg) {
    case UiMenuPage::Wifi: {
      char l1[48], l2[48];
      l1[0] = l2[0] = '\0';
      if (H->wifi_status) H->wifi_status(l1, sizeof(l1), l2, sizeof(l2));
      snprintf(buf, sizeof(buf), "%s", l1);
      return buf;
    }
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
  } else if (it.act == Act::Phone) {
    ui_menu_close();
    if (H->open_center_page) H->open_center_page(4);
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

// ---------------------------------------------------------------- Wi-Fi
void wifi_refresh_status() {
  if (!s_wifi_status) {
    return;
  }
  char l1[64], l2[64];
  l1[0] = l2[0] = '\0';
  if (H->wifi_status) H->wifi_status(l1, sizeof(l1), l2, sizeof(l2));
  if (strcmp(lv_label_get_text(s_wifi_status), l1) != 0) lv_label_set_text(s_wifi_status, l1);
  if (strcmp(lv_label_get_text(s_wifi_status2), l2) != 0) lv_label_set_text(s_wifi_status2, l2);
}

void wifi_net_clicked(lv_event_t *e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_scan_n || !H->wifi_join) {
    return;
  }
  H->wifi_join(s_scan[i].ssid, s_scan[i].open);
  wifi_refresh_status();
}

void wifi_fill_list() {
  if (!s_wifi_list) {
    return;
  }
  lv_obj_clean(s_wifi_list);
  for (int i = 0; i < s_scan_n; i++) {
    lv_obj_t *row = lv_obj_create(s_wifi_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 440, 64);
    lv_obj_set_style_radius(row, 18, 0);
    fill(row, T.surface);
    outline(row, T.line, 2);
    lv_obj_set_style_bg_color(row, T.line, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, wifi_net_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *ic = label(row, IC_WIFI, &elc_icons_34, T.dim);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t *l = label(row, s_scan[i].ssid, T.sub, T.text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 300);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 66, 0);
    if (!s_scan[i].open) {
      lv_obj_t *lk = label(row, IC_LOCK, &elc_icons_34, T.faint);
      lv_obj_align(lk, LV_ALIGN_RIGHT_MID, -16, 0);
    }
  }
}

void build_wifi(lv_obj_t *page) {
  page_title(page, "Wi-Fi");
  lv_obj_t *card = box(page, 170, 176, 460, 96);
  lv_obj_set_style_radius(card, 22, 0);
  fill(card, T.surface);
  outline(card, T.line, 2);
  s_wifi_status = label(card, "", T.body, T.text);
  lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_wifi_status, 420);
  lv_obj_align(s_wifi_status, LV_ALIGN_TOP_LEFT, 20, 14);
  s_wifi_status2 = label(card, "", T.sub, T.dim);
  lv_label_set_long_mode(s_wifi_status2, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_wifi_status2, 420);
  lv_obj_align(s_wifi_status2, LV_ALIGN_TOP_LEFT, 20, 56);

  pill(page, 170, 288, 222, 56, "Join saved", [](lv_event_t *) {
    if (H->wifi_has_saved && H->wifi_has_saved() && H->wifi_join_saved) {
      H->wifi_join_saved();
      wifi_refresh_status();
    }
  });
  if (H->local_wifi) {
    lv_obj_t *scan = pill(page, 408, 288, 222, 56, "Scan", [](lv_event_t *) {
      if (s_scanning || !H->wifi_scan_start) return;
      s_scanning = true;
      H->wifi_scan_start();
      if (s_wifi_scan_lbl) lv_label_set_text(s_wifi_scan_lbl, "Scanning\xE2\x80\xA6");
    });
    s_wifi_scan_lbl = lv_obj_get_child(scan, 0);
    s_wifi_list = lv_obj_create(page);
    lv_obj_remove_style_all(s_wifi_list);
    lv_obj_set_pos(s_wifi_list, 180, 360);
    lv_obj_set_size(s_wifi_list, 440, 300);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, 10, 0);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_OFF);
    wifi_fill_list();
  } else {
    // The radio is on the center: scanning and password entry happen there.
    pill(page, 408, 288, 222, 56, "Open on center", [](lv_event_t *) {
      if (H->open_center_page) H->open_center_page(1);
    });
    lv_obj_t *cap = label(page, "Scanning and passwords\nopen on the center screen.", T.sub, T.faint);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 384);
  }
  add_back(page);
  wifi_refresh_status();
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
const char *const kSvcNames[8] = {"Sensor link", "Odometer", "Faults", "GPS", "Wi-Fi", "Audio bridge", "Left screen", "Right screen"};

void svc_refresh() {
  for (int i = 0; i < 8; i++) {
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
  for (int i = 0; i < 8; i++) {
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

// ---------------------------------------------------------------- lifecycle
void clear_page_refs() {
  s_wifi_status = s_wifi_status2 = s_wifi_list = s_wifi_scan_lbl = nullptr;
  s_bright_arc = s_bright_val = s_follow_sw = nullptr;
  memset(s_svc_val, 0, sizeof(s_svc_val));
  memset(s_svc_dot, 0, sizeof(s_svc_dot));
  memset(s_screen_btn, 0, sizeof(s_screen_btn));
  s_demo_btn_lbl = nullptr;
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
  } else if (s_page_id == UiMenuPage::Wifi) {
    if (s_scanning && H->wifi_scan_poll) {
      int n = 0;
      const int r = H->wifi_scan_poll(s_scan, 8, &n);
      if (r != 0) {
        s_scanning = false;
        s_scan_n = (r == 1) ? n : 0;
        if (s_wifi_scan_lbl) lv_label_set_text(s_wifi_scan_lbl, s_scan_n ? "Scan" : "Scan (none found)");
        wifi_fill_list();
      }
    }
    if (H->wifi_poll) H->wifi_poll();
    if (slow) wifi_refresh_status();
  } else if (s_page_id == UiMenuPage::Service) {
    if (slow) svc_refresh();
  } else if (s_page_id == UiMenuPage::Screens) {
    if (screens_sig() != s_screens_sig) screens_restyle();
  }
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
  if (!H) {
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
    case UiMenuPage::Wifi: build_wifi(s_page); break;
    case UiMenuPage::Brightness: build_brightness(s_page); break;
    case UiMenuPage::Theme: build_theme(s_page); break;
    case UiMenuPage::Screens: build_screens(s_page); break;
    case UiMenuPage::Service: build_service(s_page); break;
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
  p4_screen_set_tear_free(true);
  s_dragging = false;
  s_scanning = false;
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
  }
}

void ui_menu_press() {
  if (!s_root) {
    ui_menu_open(UiMenuPage::Home);
  } else if (s_page_id == UiMenuPage::Home) {
    activate(s_target);
  } else {
    ui_menu_open(UiMenuPage::Home);
  }
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
