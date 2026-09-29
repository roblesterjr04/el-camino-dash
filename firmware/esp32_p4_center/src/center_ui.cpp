#include "center_ui.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <canvas/Arduino_Canvas.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <WiFi.h>

#include <cluster_i2c.h>
#include <elc_can_codec.h>
#include <sensor_link.h>
#include <sensor_packet.h>

#include <p4_round_mask.h>

#include "FreeSansBold10pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "center_ble.h"
#include "center_can.h"
#include "center_audio_link.h"
#include "center_gps.h"
#include "center_menu.h"
#include "center_sd_settings.h"
#include "center_wifi_ntp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint16_t ui_rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

enum class UiPage : uint8_t {
  Closed = 0,
  Home,
  FsPhone,
  FsPsk,
  FsSerial,
  FsGpsDiag,
  FsWifiScan,
  FsWifiConn,
};

enum KSpec : int16_t {
  KK_JOIN = -1000,
  KK_BS = -1001,
  KK_CANCEL = -1002,
  KK_SPACE = -1003,
  KK_SHIFT = -1004,
};

struct KeyRect {
  int16_t x, y, w, h;
  int16_t code;
};

static constexpr int16_t kLayoutInset = 120;
static constexpr int kMaxKeys = 56;
static KeyRect s_keys[kMaxKeys];
static int s_key_n = 0;

static Arduino_GFX *s_gfx = nullptr;
static Preferences *s_prefs = nullptr;
static UiPage s_page = UiPage::Closed;
static CenterViewMode s_view = CenterViewMode::Tach;
static ClusterTheme s_theme = ClusterTheme::Classic;
// Screens (ELC_CAN_SCREEN_*): which one has the menu, which one shows media. media_shown is for a side screen
// (on the center, media shown = the Media view).
static uint8_t s_iface = ELC_CAN_SCREEN_CENTER;
static uint8_t s_media = ELC_CAN_SCREEN_CENTER;
static bool s_media_shown = false;

static char s_scan_ssid[8][33];
static uint8_t s_scan_enc[8];
static int s_scan_n = 0;

static char s_pending_ssid[33];
static char s_pwd[65];
static int s_pwd_len = 0;
static bool s_kb_shift = false;

static Arduino_Canvas *s_fs_canvas = nullptr;

static constexpr int kSensorLogLines = 22;
static constexpr int kSensorLogChars = 120;
static char s_sensor_log[kSensorLogLines][kSensorLogChars];
static int s_sensor_log_count = 0;
static uint32_t s_last_sensor_pkt_ms = 0;
static uint32_t s_sensor_pkts_since_stats = 0;
static uint32_t s_last_sensor_stats_ms = 0;


static void sensor_log_push_line(const char *line) {
  if (s_sensor_log_count < kSensorLogLines) {
    strncpy(s_sensor_log[s_sensor_log_count], line, (size_t)kSensorLogChars - 1u);
    s_sensor_log[s_sensor_log_count][kSensorLogChars - 1] = '\0';
    s_sensor_log_count++;
  } else {
    memmove(s_sensor_log[0], s_sensor_log[1], sizeof(s_sensor_log[0]) * (size_t)(kSensorLogLines - 1));
    strncpy(s_sensor_log[kSensorLogLines - 1], line, (size_t)kSensorLogChars - 1u);
    s_sensor_log[kSensorLogLines - 1][kSensorLogChars - 1] = '\0';
  }
}

static bool touch_in_circle(int16_t x, int16_t y) {
  if (!s_gfx) {
    return false;
  }
  const int16_t w = s_gfx->width();
  const int16_t h = s_gfx->height();
  const int32_t cx = (int32_t)w / 2;
  const int32_t cy = (int32_t)h / 2;
  const int32_t r = ((int32_t)(w < h ? w : h) - 6) / 2;
  const int32_t dx = (int32_t)x - cx;
  const int32_t dy = (int32_t)y - cy;
  return dx * dx + dy * dy <= r * r;
}

static void ensure_fs_canvas(Arduino_GFX *parent) {
  const int16_t w = parent->width();
  const int16_t h = parent->height();
  if (!s_fs_canvas || s_fs_canvas->width() != w || s_fs_canvas->height() != h) {
    delete s_fs_canvas;
    s_fs_canvas = new Arduino_Canvas(w, h, parent);
    if (s_fs_canvas && !s_fs_canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
      delete s_fs_canvas;
      s_fs_canvas = nullptr;
    }
  }
}

static void blit_round_canvas(Arduino_GFX *parent) {
  if (!s_fs_canvas) {
    return;
  }
  uint16_t *buf = s_fs_canvas->getFramebuffer();
  if (!buf) {
    return;
  }
  p4_rgb565_mask_outside_circle(buf, s_fs_canvas->width(), s_fs_canvas->height(), 0x0000);
  parent->draw16bitRGBBitmap(0, 0, buf, s_fs_canvas->width(), s_fs_canvas->height());
}

static void layout_wifi_keyboard(int16_t sw, int16_t sh) {
  s_key_n = 0;
  const int16_t gap = 4;
  const int cols = 10;
  const int16_t inner = (int16_t)(sw - 2 * kLayoutInset);
  const int16_t kw = (int16_t)((inner - gap * (cols - 1)) / cols);
  const int16_t kh = 36;
  int16_t y = (int16_t)(sh - kLayoutInset - 240);

  auto add_row_chars = [&](const char *s) {
    const int n = (int)strlen(s);
    const int16_t roww = (int16_t)(n * kw + (n - 1) * gap);
    int16_t x0 = (int16_t)(kLayoutInset + (inner - roww) / 2);
    for (int i = 0; i < n && s_key_n < kMaxKeys; i++) {
      s_keys[s_key_n++] = {x0, y, kw, kh, (int16_t)(uint8_t)s[i]};
      x0 = (int16_t)(x0 + kw + gap);
    }
    y = (int16_t)(y + kh + gap);
  };

  add_row_chars("1234567890");
  add_row_chars("qwertyuiop");
  add_row_chars("asdfghjkl");
  {
    const char *z = "zxcvbnm";
    const int n = (int)strlen(z);
    const int16_t wshift = (int16_t)(kw + 20);
    const int16_t zw = (int16_t)(n * kw + (n - 1) * gap + gap + kw);
    const int16_t roww = (int16_t)(wshift + gap + zw);
    int16_t x0 = (int16_t)(kLayoutInset + (inner - roww) / 2);
    if (s_key_n < kMaxKeys) {
      s_keys[s_key_n++] = {x0, y, wshift, kh, KK_SHIFT};
    }
    x0 = (int16_t)(x0 + wshift + gap);
    for (int i = 0; i < n && s_key_n < kMaxKeys; i++) {
      s_keys[s_key_n++] = {x0, y, kw, kh, (int16_t)(uint8_t)z[i]};
      x0 = (int16_t)(x0 + kw + gap);
    }
    if (s_key_n < kMaxKeys) {
      s_keys[s_key_n++] = {x0, y, kw, kh, KK_BS};
    }
    y = (int16_t)(y + kh + gap);
  }
  {
    const int16_t tri_gap = 8;
    const int16_t wsp = (int16_t)((inner - 2 * tri_gap) * 55 / 100);
    const int16_t wj = (int16_t)((inner - 2 * tri_gap) * 22 / 100);
    const int16_t wc = (int16_t)(inner - wsp - wj - 2 * tri_gap);
    int16_t x0 = kLayoutInset;
    s_keys[s_key_n++] = {x0, y, wsp, (int16_t)(kh + 6), KK_SPACE};
    x0 = (int16_t)(x0 + wsp + tri_gap);
    s_keys[s_key_n++] = {x0, y, wj, (int16_t)(kh + 6), KK_JOIN};
    x0 = (int16_t)(x0 + wj + tri_gap);
    if (s_key_n < kMaxKeys) {
      s_keys[s_key_n++] = {x0, y, wc, (int16_t)(kh + 6), KK_CANCEL};
    }
  }
}

static int16_t hit_key(int16_t tx, int16_t ty) {
  for (int i = 0; i < s_key_n; i++) {
    const KeyRect &k = s_keys[i];
    if (tx >= k.x && tx < k.x + k.w && ty >= k.y && ty < k.y + k.h) {
      return k.code;
    }
  }
  return 0;
}

static bool hit_cancel_bar(int16_t tx, int16_t ty, int16_t sw, int16_t sh) {
  const int16_t bw = 280;
  const int16_t bh = 48;
  const int16_t bx = (int16_t)((sw - bw) / 2);
  const int16_t by = (int16_t)(sh - kLayoutInset - 8 - bh);
  return tx >= bx && tx < bx + bw && ty >= by && ty < by + bh;
}

/** Leave any legacy full-screen page and show the LVGL menu. */
static void menu_open_home(CenterMenuPage page = CenterMenuPage::Home) {
  s_page = UiPage::Closed;
  s_kb_shift = false;
  center_shell_set_wifi_psk_pending(nullptr);
  center_menu_open(page);
}

static void close_to_gauge() {
  center_menu_close();
  s_page = UiPage::Closed;
  s_kb_shift = false;
  center_shell_set_wifi_psk_pending(nullptr);
}

void center_ui_sensor_log_line(const char *line) {
  if (line && line[0]) {
    sensor_log_push_line(line);
  }
}

void center_ui_on_sensor_uart_line(const char *line) {
  center_ui_sensor_log_line(line);
}

void center_ui_on_sensor_packet(const SensorPacketValues &v) {
  s_last_sensor_pkt_ms = millis();
  s_sensor_pkts_since_stats++;
  char line[kSensorLogChars];
  if (v.extended && v.has_odometer && v.has_fuel_economy) {
    const double odo_mi = (double)v.odometer_tenths_mile * 0.1;
    const double dte_mi = (double)v.dte_tenths_mile * 0.1;
    snprintf(line, sizeof(line),
             "pot=%u th=%u amb=%u p=%u v=%u spd=%lu eng=%lu f=0x%02X x=0x%04X odo=%.1fmi mpg_i=%u.%u mpg_a=%u.%u dte=%.1fmi",
             (unsigned)v.pot_adc, (unsigned)v.therm_adc, (unsigned)v.ambient_adc, (unsigned)v.pressure_adc,
             (unsigned)v.voltage_adc, (unsigned long)v.speed_rpm, (unsigned long)v.engine_rpm, (unsigned)v.flags,
             (unsigned)v.faults, odo_mi, (unsigned)(v.mpg_instant_x10 / 10u), (unsigned)(v.mpg_instant_x10 % 10u),
             (unsigned)(v.mpg_average_x10 / 10u), (unsigned)(v.mpg_average_x10 % 10u), dte_mi);
  } else if (v.extended && v.has_odometer) {
    const double odo_mi = (double)v.odometer_tenths_mile * 0.1;
    snprintf(line, sizeof(line),
             "pot=%u th=%u amb=%u p=%u v=%u spd=%lu eng=%lu f=0x%02X x=0x%04X odo=%.1fmi odo_t=%lu",
             (unsigned)v.pot_adc, (unsigned)v.therm_adc, (unsigned)v.ambient_adc, (unsigned)v.pressure_adc,
             (unsigned)v.voltage_adc, (unsigned long)v.speed_rpm, (unsigned long)v.engine_rpm, (unsigned)v.flags,
             (unsigned)v.faults, odo_mi, (unsigned long)v.odometer_tenths_mile);
  } else if (v.extended) {
    snprintf(line, sizeof(line), "pot=%u th=%u amb=%u p=%u v=%u spd=%lu eng=%lu f=0x%02X x=0x%04X",
             (unsigned)v.pot_adc, (unsigned)v.therm_adc, (unsigned)v.ambient_adc, (unsigned)v.pressure_adc,
             (unsigned)v.voltage_adc, (unsigned long)v.speed_rpm, (unsigned long)v.engine_rpm, (unsigned)v.flags,
             (unsigned)v.faults);
  } else {
    snprintf(line, sizeof(line), "pot=%u th=%u p=%u v=%u spd=%lu eng=%lu f=0x%02X x=0x%04X", (unsigned)v.pot_adc,
             (unsigned)v.therm_adc, (unsigned)v.pressure_adc, (unsigned)v.voltage_adc, (unsigned long)v.speed_rpm,
             (unsigned long)v.engine_rpm, (unsigned)v.flags, (unsigned)v.faults);
  }
  sensor_log_push_line(line);
}

void center_ui_begin(Arduino_GFX *gfx, Preferences *prefs) {
  s_gfx = gfx;
  s_prefs = prefs;
  center_menu_begin(prefs);
  if (prefs) {
    s_theme = cluster_theme_from_reg(prefs->getUChar("ctheme", 0));
    uint8_t v = prefs->getUChar("view", 0);
    if (v > 3) {
      v = 0;
    }
    s_view = (CenterViewMode)v;
    s_iface = prefs->getUChar("iface", ELC_CAN_SCREEN_CENTER);
    s_media = prefs->getUChar("media", ELC_CAN_SCREEN_CENTER);
    s_media_shown = prefs->getBool("mshown", false);
  }
  if (s_iface > ELC_CAN_SCREEN_RIGHT) s_iface = ELC_CAN_SCREEN_CENTER;
  if (s_media > ELC_CAN_SCREEN_RIGHT) s_media = ELC_CAN_SCREEN_CENTER;
  if (s_media != ELC_CAN_SCREEN_CENTER && s_view == CenterViewMode::Media) {
    s_view = CenterViewMode::Tach;  // media lives on a side screen
  }
  center_audio_link_set_fetch_art(s_media == ELC_CAN_SCREEN_CENTER);
}

CenterViewMode center_ui_view_mode() { return s_view; }

ClusterTheme center_ui_cluster_theme() { return s_theme; }

uint8_t center_ui_interface_screen() { return s_iface; }
uint8_t center_ui_media_screen() { return s_media; }
bool center_ui_media_shown() {
  return s_media == ELC_CAN_SCREEN_CENTER ? s_view == CenterViewMode::Media : s_media_shown;
}

void center_ui_sync_cluster_bus() {
  cluster_i2c_broadcast_center_theme(s_theme, (ClusterCenterSubview)(uint8_t)s_view);
  ElcCanClusterState st;
  st.theme = (uint8_t)s_theme;
  st.center_subview = (uint8_t)s_view;
  st.interface_screen = s_iface;
  st.media_screen = s_media;
  st.brightness = center_menu_brightness();
  st.flags = (uint8_t)((center_menu_dim_with_lights() ? ELC_CAN_CLUSTER_DIM_WITH_LIGHTS : 0) |
                       (center_ui_media_shown() ? ELC_CAN_CLUSTER_MEDIA_SHOWN : 0));
  center_can_send_cluster(st);
}

static void save_screens() {
  if (s_prefs) {
    s_prefs->putUChar("iface", s_iface);
    s_prefs->putUChar("media", s_media);
    s_prefs->putBool("mshown", s_media_shown);
    center_sd_settings_sync(*s_prefs);
  }
}

void center_ui_set_interface_screen(uint8_t screen) {
  if (screen > ELC_CAN_SCREEN_RIGHT || screen == s_iface) {
    return;
  }
  s_iface = screen;
  if (screen != ELC_CAN_SCREEN_CENTER) {
    center_menu_close();  // the menu moves to that screen (as a side screen closes its menu when it moves away)
  }
  save_screens();
  center_ui_sync_cluster_bus();
}

void center_ui_set_media_screen(uint8_t screen) {
  if (screen > ELC_CAN_SCREEN_RIGHT || screen == s_media) {
    return;
  }
  // Media moves with what it was doing: shown here stays shown there.
  const bool shown = center_ui_media_shown();
  s_media = screen;
  s_media_shown = screen != ELC_CAN_SCREEN_CENTER && shown;
  if (screen == ELC_CAN_SCREEN_CENTER) {
    if (shown) s_view = CenterViewMode::Media;
  } else if (s_view == CenterViewMode::Media) {
    s_view = CenterViewMode::Tach;
  }
  if (s_prefs) s_prefs->putUChar("view", (uint8_t)s_view);
  center_audio_link_set_fetch_art(s_media == ELC_CAN_SCREEN_CENTER);
  save_screens();
  center_ui_sync_cluster_bus();
}

void center_ui_show_media(bool on) {
  if (s_media == ELC_CAN_SCREEN_CENTER) {
    center_ui_set_view(on ? CenterViewMode::Media : CenterViewMode::Tach);
    return;
  }
  if (s_media_shown == on) {
    return;
  }
  s_media_shown = on;
  save_screens();
  center_ui_sync_cluster_bus();
}

/** Next view for the 3 s hold: Media is in the cycle only when the center is the media screen. */
static CenterViewMode next_view(CenterViewMode v) {
  uint8_t n = (uint8_t)(((uint8_t)v + 1u) % 4u);
  if (n == (uint8_t)CenterViewMode::Media && s_media != ELC_CAN_SCREEN_CENTER) {
    n = (uint8_t)CenterViewMode::Efficiency;
  }
  return (CenterViewMode)n;
}

void center_ui_apply_setting(const ElcCanSettingRequest &r) {
  switch (r.setting) {
    case ELC_CAN_SET_THEME:
      if (r.value <= 2) center_ui_set_theme(cluster_theme_from_reg(r.value));
      break;
    case ELC_CAN_SET_CENTER_VIEW:
      if (r.value <= 3) center_ui_set_view((CenterViewMode)r.value);
      break;
    case ELC_CAN_SET_INTERFACE_SCREEN: center_ui_set_interface_screen(r.value); break;
    case ELC_CAN_SET_MEDIA_SCREEN: center_ui_set_media_screen(r.value); break;
    case ELC_CAN_SET_BRIGHTNESS: center_menu_set_brightness(r.value, center_menu_dim_with_lights(), true); break;
    case ELC_CAN_SET_DIM_WITH_LIGHTS: center_menu_set_brightness(center_menu_brightness(), r.value != 0, true); break;
    case ELC_CAN_SET_MEDIA_SHOWN: center_ui_show_media(r.value != 0); break;
    case ELC_CAN_SET_DEMO: center_set_demo(r.value != 0); break;
    case ELC_CAN_ACT_WIFI_JOIN_SAVED: center_menu_wifi_join_saved(); break;
    case ELC_CAN_ACT_OPEN_CENTER_PAGE: center_menu_open_center_page(r.value); break;
    default: break;
  }
  center_ui_sync_cluster_bus();  // answer every request, so the asker sees the outcome even when nothing changed
}

ClusterTheme center_ui_next_theme(ClusterTheme t) {
  return t == ClusterTheme::Classic ? ClusterTheme::Modern : (t == ClusterTheme::Modern ? ClusterTheme::Fallout : ClusterTheme::Classic);
}

void center_ui_set_view(CenterViewMode v) {
  if (v == CenterViewMode::Media && s_media != ELC_CAN_SCREEN_CENTER) {
    center_ui_show_media(true);  // media is shown on its own screen
    return;
  }
  s_view = v;
  if (s_prefs) {
    s_prefs->putUChar("view", (uint8_t)v);
    center_sd_settings_sync(*s_prefs);
  }
  center_ui_sync_cluster_bus();
}

void center_ui_set_theme(ClusterTheme t) {
  s_theme = t;
  if (s_prefs) {
    s_prefs->putUChar("ctheme", (uint8_t)t);
    center_sd_settings_sync(*s_prefs);
  }
  center_ui_sync_cluster_bus();
}

void center_ui_open_legacy_page(CenterLegacyPage p) {
  center_menu_close();
  s_page = p == CenterLegacyPage::Phone ? UiPage::FsPhone : (p == CenterLegacyPage::SensorLog ? UiPage::FsSerial : UiPage::FsGpsDiag);
}

void center_ui_open_wifi_password(const char *ssid) {
  center_menu_close();
  strncpy(s_pending_ssid, ssid ? ssid : "", 32);
  s_pending_ssid[32] = '\0';
  center_shell_set_wifi_psk_pending(s_pending_ssid);
  s_pwd_len = 0;
  s_pwd[0] = '\0';
  s_kb_shift = false;
  s_page = UiPage::FsPsk;
  layout_wifi_keyboard(s_gfx->width(), s_gfx->height());
}

void center_ui_process_touch_gesture(uint32_t duration_ms, int16_t x, int16_t y) {
  if (!s_gfx || !s_prefs || center_menu_is_open()) {
    return;  // the LVGL menu handles its own touches
  }
  if (!touch_in_circle(x, y)) {
    return;
  }
  if (center_ui_fullscreen()) {
    if (duration_ms < 3000u) {
      center_ui_on_touch_release(x, y);
    }
    return;
  }
  if (duration_ms >= 10000u) {
    center_ui_set_theme(center_ui_next_theme(s_theme));
    return;
  }
  if (duration_ms >= 3000u) {
    center_ui_set_view(next_view(s_view));
    return;
  }
  center_ui_on_touch_release(x, y);
}

bool center_ui_overlay_open() { return center_menu_is_open(); }

bool center_ui_fullscreen() {
  return s_page == UiPage::FsPsk || s_page == UiPage::FsSerial || s_page == UiPage::FsGpsDiag ||
         s_page == UiPage::FsWifiScan || s_page == UiPage::FsWifiConn || s_page == UiPage::FsPhone;
}

static void draw_scan_progress(Arduino_GFX *t, int16_t sw, int16_t sh, uint32_t ms, float phase01) {
  const int16_t cx = (int16_t)(sw / 2);
  const int16_t cy = (int16_t)(sh / 2 - 40);
  const int16_t rr = (int16_t)(sw < sh ? sw : sh) / 7;
  const float a0 = (float)((ms % 2000) * (2.0 * M_PI / 2000.0));
  for (int i = 0; i < 12; i++) {
    const float a = a0 + (float)i * (2.0f * (float)M_PI / 12.0f);
    const int16_t x1 = (int16_t)((float)cx + cosf(a) * (float)(rr - 16));
    const int16_t y1 = (int16_t)((float)cy + sinf(a) * (float)(rr - 16));
    const int16_t x2 = (int16_t)((float)cx + cosf(a) * (float)rr);
    const int16_t y2 = (int16_t)((float)cy + sinf(a) * (float)rr);
    t->drawLine(x1, y1, x2, y2, 0xFFFFu);
  }
  const int16_t barw = (int16_t)(sw - 2 * kLayoutInset);
  const int16_t bw = (int16_t)((float)barw * phase01);
  t->fillRoundRect(kLayoutInset, (int16_t)(cy + rr + 20), barw, 10, 4, ui_rgb565(40, 40, 40));
  t->fillRoundRect(kLayoutInset, (int16_t)(cy + rr + 20), bw, 10, 4, ui_rgb565(200, 200, 200));
}

static void ui_print_trunc_line(Arduino_GFX *t, int16_t x, int16_t y, int16_t max_chars, uint16_t col,
                                const char *s) {
  t->setFont(nullptr);
  t->setTextSize(1);
  t->setTextColor(col);
  t->setCursor(x, y);
  if (!s || !s[0]) {
    t->print("—");
    return;
  }
  const size_t mc = (size_t)(max_chars > 8 ? max_chars : 8);
  char b[128];
  strncpy(b, s, sizeof(b) - 1u);
  b[sizeof(b) - 1u] = '\0';
  const size_t L = strlen(b);
  if (L > mc) {
    const size_t cut = mc > 3u ? mc - 3u : 0u;
    if (cut < sizeof(b)) {
      b[cut] = '\0';
    }
    const size_t len2 = strlen(b);
    if (len2 + 3u < sizeof(b)) {
      memcpy(b + len2, "...", 4);
    }
  }
  t->print(b);
}

static void ui_print_trunc_gfx(Arduino_GFX *t, int16_t x, int16_t y, int16_t max_chars, uint16_t col,
                               const GFXfont *font, const char *s) {
  t->setFont(font);
  t->setTextSize(1);
  t->setTextColor(col);
  t->setCursor(x, y);
  if (!s || !s[0]) {
    t->print("—");
    return;
  }
  const size_t mc = (size_t)(max_chars > 8 ? max_chars : 8);
  char b[128];
  strncpy(b, s, sizeof(b) - 1u);
  b[sizeof(b) - 1u] = '\0';
  const size_t L = strlen(b);
  if (L > mc) {
    const size_t cut = mc > 3u ? mc - 3u : 0u;
    if (cut < sizeof(b)) {
      b[cut] = '\0';
    }
    const size_t len2 = strlen(b);
    if (len2 + 3u < sizeof(b)) {
      memcpy(b + len2, "...", 4);
    }
  }
  t->print(b);
}

static int16_t ui_draw_wrapped_gfx(Arduino_GFX *t, const GFXfont *font, int16_t x0, int16_t y0, int16_t y_max,
                                   int16_t max_chars, int16_t line_h, uint16_t color, const char *text) {
  t->setFont(font);
  t->setTextSize(1);
  t->setTextColor(color);
  int16_t y = y0;
  if (!text || text[0] == '\0') {
    t->setCursor(x0, y);
    t->print("(empty)");
    return (int16_t)(y + line_h);
  }
  if (max_chars < 12) {
    max_chars = 12;
  }
  const size_t len = strlen(text);
  size_t pos = 0;
  while (pos < len && y <= (int16_t)(y_max - line_h)) {
    size_t chunk = (size_t)max_chars;
    if (pos + chunk > len) {
      chunk = len - pos;
    }
    char buf[160];
    if (chunk >= sizeof(buf)) {
      chunk = sizeof(buf) - 1u;
    }
    memcpy(buf, text + pos, chunk);
    buf[chunk] = '\0';
    t->setCursor(x0, y);
    t->print(buf);
    y = (int16_t)(y + line_h);
    pos += chunk;
  }
  return y;
}

/** Default-font wrapped text; returns Y just below last printed line. */
static int16_t ui_draw_wrapped_small(Arduino_GFX *t, int16_t x0, int16_t y0, int16_t y_max, int16_t max_chars,
                                     int16_t line_h, uint16_t color, const char *text) {
  t->setFont(nullptr);
  t->setTextSize(1);
  t->setTextColor(color);
  int16_t y = y0;
  if (!text || text[0] == '\0') {
    t->setCursor(x0, y);
    t->print("(empty)");
    return (int16_t)(y + line_h);
  }
  if (max_chars < 12) {
    max_chars = 12;
  }
  const size_t len = strlen(text);
  size_t pos = 0;
  while (pos < len && y <= (int16_t)(y_max - line_h)) {
    size_t chunk = (size_t)max_chars;
    if (pos + chunk > len) {
      chunk = len - pos;
    }
    char buf[160];
    if (chunk >= sizeof(buf)) {
      chunk = sizeof(buf) - 1u;
    }
    memcpy(buf, text + pos, chunk);
    buf[chunk] = '\0';
    t->setCursor(x0, y);
    t->print(buf);
    y = (int16_t)(y + line_h);
    pos += chunk;
  }
  return y;
}

void center_ui_draw_fullscreen(Arduino_GFX *gfx) {
  if (!gfx) {
    return;
  }
  ensure_fs_canvas(gfx);
  if (!s_fs_canvas) {
    return;
  }
  Arduino_GFX *t = s_fs_canvas;
  const int16_t sw = t->width();
  const int16_t sh = t->height();
  const uint32_t ms = millis();

  t->fillScreen(0x0000);
  t->setFont(&FreeSansBold10pt7b);
  t->setTextSize(1);
  t->setTextColor(0xFFFFu);

  if (s_page == UiPage::FsWifiScan) {
    t->setCursor(kLayoutInset, kLayoutInset + 10);
    t->print("Scanning Wi-Fi");
    const float ph = 0.35f + 0.65f * (0.5f + 0.5f * sinf((float)ms * 0.004f));
    draw_scan_progress(t, sw, sh, ms, ph);
    t->setCursor(kLayoutInset, (int16_t)(sh / 2 + 80));
    t->setTextColor(ui_rgb565(180, 180, 180));
    t->print("Searching for networks…");
    t->setTextColor(0xFFFFu);
    const int16_t bw = 280;
    const int16_t bh = 48;
    const int16_t bx = (int16_t)((sw - bw) / 2);
    const int16_t by = (int16_t)(sh - kLayoutInset - 8 - bh);
    t->fillRoundRect(bx, by, bw, bh, 10, ui_rgb565(48, 48, 48));
    t->drawRoundRect(bx, by, bw, bh, 10, ui_rgb565(140, 140, 140));
    int16_t bx0 = 0, by0 = 0;
    uint16_t tw = 0, th = 0;
    t->getTextBounds("CANCEL", 0, 0, &bx0, &by0, &tw, &th);
    t->setCursor((int16_t)(bx + (bw - (int16_t)tw) / 2), (int16_t)(by + (bh + (int16_t)th) / 2));
    t->print("CANCEL");
  } else if (s_page == UiPage::FsWifiConn) {
    t->setCursor(kLayoutInset, kLayoutInset + 10);
    t->print("Connecting…");
    const uint32_t el = ms % 5000u;
    draw_scan_progress(t, sw, sh, ms, (float)el / 5000.0f);
    t->setCursor(kLayoutInset, (int16_t)(sh / 2 + 60));
    t->setTextColor(ui_rgb565(180, 180, 180));
    t->print("Joining access point");
    t->setTextColor(0xFFFFu);
    {
      const int16_t bw = 280;
      const int16_t bh = 48;
      const int16_t bx = (int16_t)((sw - bw) / 2);
      const int16_t by = (int16_t)(sh - kLayoutInset - 8 - bh);
      t->fillRoundRect(bx, by, bw, bh, 10, ui_rgb565(48, 48, 48));
      t->drawRoundRect(bx, by, bw, bh, 10, ui_rgb565(140, 140, 140));
      int16_t bx0 = 0, by0 = 0;
      uint16_t tw = 0, th = 0;
      t->getTextBounds("CANCEL", 0, 0, &bx0, &by0, &tw, &th);
      t->setCursor((int16_t)(bx + (bw - (int16_t)tw) / 2), (int16_t)(by + (bh + (int16_t)th) / 2));
      t->print("CANCEL");
    }
  } else if (s_page == UiPage::FsPsk) {
    t->setCursor(kLayoutInset, kLayoutInset);
    t->print("WPA password");
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 28));
    t->setTextColor(ui_rgb565(160, 160, 200));
    t->print(s_pending_ssid);
    t->setTextColor(0xFFFFu);
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 56));
    if (s_pwd_len == 0) {
      t->setTextColor(ui_rgb565(140, 140, 140));
      t->print("(empty)");
      t->setTextColor(0xFFFFu);
    } else {
      t->print(s_pwd);
    }

    layout_wifi_keyboard(sw, sh);
    t->setFont(&FreeSansBold10pt7b);
    for (int i = 0; i < s_key_n; i++) {
      const KeyRect &k = s_keys[i];
      const bool shift_hi = (k.code == KK_SHIFT && s_kb_shift);
      t->fillRoundRect(k.x, k.y, k.w, k.h, 6, shift_hi ? ui_rgb565(55, 55, 75) : ui_rgb565(36, 36, 36));
      t->drawRoundRect(k.x, k.y, k.w, k.h, 6, shift_hi ? ui_rgb565(160, 160, 200) : ui_rgb565(100, 100, 100));
      char lbl[8] = {0};
      if (k.code == KK_BS) {
        strncpy(lbl, "<", sizeof(lbl));
      } else if (k.code == KK_SHIFT) {
        strncpy(lbl, "Shift", sizeof(lbl) - 1);
      } else if (k.code == KK_SPACE) {
        strncpy(lbl, "_", sizeof(lbl));
      } else if (k.code == KK_JOIN) {
        strncpy(lbl, "JOIN", sizeof(lbl));
      } else if (k.code == KK_CANCEL) {
        strncpy(lbl, "EXIT", sizeof(lbl));
      } else if (k.code > 0) {
        char ch = (char)k.code;
        if (s_kb_shift && ch >= 'a' && ch <= 'z') {
          ch = (char)toupper((unsigned char)ch);
        }
        lbl[0] = ch;
        lbl[1] = 0;
      }
      int16_t bx0 = 0, by0 = 0;
      uint16_t tw = 0, th = 0;
      t->getTextBounds(lbl, 0, 0, &bx0, &by0, &tw, &th);
      t->setCursor((int16_t)(k.x + (k.w - (int16_t)tw) / 2), (int16_t)(k.y + (k.h + (int16_t)th) / 2));
      t->print(lbl);
    }
    t->setFont(nullptr);
  } else if (s_page == UiPage::FsPhone) {
    t->setFont(&FreeSansBold10pt7b);
    t->setTextSize(1);
    t->setTextColor(0xFFFFu);
    t->setCursor(kLayoutInset, kLayoutInset);
    t->print("Phone");
    t->setFont(nullptr);
    t->setTextSize(1);
    t->setTextColor(ui_rgb565(150, 150, 160));
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 26));
    t->print("Hands-free / cabin audio — integration TBD.");
    t->setTextColor(0xFFFFu);
    t->setFont(&FreeSansBold10pt7b);
    const int16_t bw = 280;
    const int16_t bh = 48;
    const int16_t bx = (int16_t)((sw - bw) / 2);
    const int16_t by = (int16_t)(sh - kLayoutInset - 8 - bh);
    t->fillRoundRect(bx, by, bw, bh, 10, ui_rgb565(48, 48, 48));
    t->drawRoundRect(bx, by, bw, bh, 10, ui_rgb565(140, 140, 140));
    int16_t bx0 = 0, by0 = 0;
    uint16_t tw = 0, th = 0;
    t->getTextBounds("BACK", 0, 0, &bx0, &by0, &tw, &th);
    t->setCursor((int16_t)(bx + (bw - (int16_t)tw) / 2), (int16_t)(by + (bh + (int16_t)th) / 2));
    t->print("BACK");
    t->setFont(nullptr);
  } else if (s_page == UiPage::FsSerial) {
    t->setCursor(kLayoutInset, kLayoutInset);
    t->print("Sensor UART");
    t->setFont(nullptr);
    t->setTextSize(1);
    t->setTextColor(ui_rgb565(150, 150, 160));
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 22));
    t->print("Decoded frames + FRAM* UART (sensor_pack on Serial1)");
    t->setTextColor(0xFFFFu);
    {
      const int16_t y0 = (int16_t)(kLayoutInset + 42);
      const int16_t lh = 14;
      const int max_lines = (int)((sh - 72 - y0) / lh);
      const int first = (s_sensor_log_count <= max_lines) ? 0 : (s_sensor_log_count - max_lines);
      for (int i = first; i < s_sensor_log_count; i++) {
        const int16_t y = (int16_t)(y0 + (i - first) * lh);
        t->setCursor(kLayoutInset, y);
        t->print(s_sensor_log[i]);
      }
      if (s_sensor_log_count == 0) {
        t->setTextColor(ui_rgb565(120, 120, 130));
        t->setCursor(kLayoutInset, (int16_t)(y0 + 4));
        t->print("(waiting for packets...)");
      }
    }
    t->setFont(&FreeSansBold10pt7b);
    t->setTextSize(1);
    t->setTextColor(0xFFFFu);
    const int16_t bw = 280;
    const int16_t bh = 48;
    const int16_t bx = (int16_t)((sw - bw) / 2);
    const int16_t by = (int16_t)(sh - kLayoutInset - 8 - bh);
    t->fillRoundRect(bx, by, bw, bh, 10, ui_rgb565(48, 48, 48));
    t->drawRoundRect(bx, by, bw, bh, 10, ui_rgb565(140, 140, 140));
    int16_t bx0 = 0, by0 = 0;
    uint16_t tw = 0, th = 0;
    t->getTextBounds("CANCEL", 0, 0, &bx0, &by0, &tw, &th);
    t->setCursor((int16_t)(bx + (bw - (int16_t)tw) / 2), (int16_t)(by + (bh + (int16_t)th) / 2));
    t->print("CANCEL");
  } else if (s_page == UiPage::FsGpsDiag) {
    const int16_t bh = 48;
    const int16_t by_btn = (int16_t)(sh - kLayoutInset - 8 - bh);
    const int16_t lh_stream = 22;
    const int16_t n_recent = CENTER_GPS_DIAG_RECENT_LINES;
    const int16_t stream_block = (int16_t)(lh_stream * (1 + n_recent) + 12);
    const int16_t y_list_top = (int16_t)(by_btn - stream_block);
    const int16_t y_hdr_max = (int16_t)(y_list_top - 8);
    const int16_t max_chars12 = (int16_t)((sw - 2 * kLayoutInset - 4) / 12);
    const int16_t lh_hdr = 24;
    int16_t y = (int16_t)(kLayoutInset + 4);

    t->setFont(&FreeSansBold12pt7b);
    t->setTextSize(1);
    t->setTextColor(0xFFFFu);
    t->setCursor(kLayoutInset, y);
    t->print("GPS UART");
    y = (int16_t)(y + 30);

    {
      char sub[88];
      snprintf(sub, sizeof(sub), "UART2 @ %lu baud", (unsigned long)center_gps_uart_baud());
      y = ui_draw_wrapped_gfx(t, &FreeSansBold12pt7b, kLayoutInset, y, y_hdr_max, max_chars12, lh_hdr,
                              ui_rgb565(140, 145, 165), sub);
    }
    {
      const uint32_t gbt = center_gps_last_uart_byte_ms();
      char rxl[72];
      if (gbt == 0) {
        strncpy(rxl, "Last RX byte: never", sizeof(rxl) - 1u);
      } else {
        snprintf(rxl, sizeof(rxl), "Last RX byte: %lums ago", (unsigned long)(ms - gbt));
      }
      rxl[sizeof(rxl) - 1u] = '\0';
      y = ui_draw_wrapped_gfx(t, &FreeSansBold12pt7b, kLayoutInset, y, y_hdr_max, max_chars12, lh_hdr,
                              ui_rgb565(140, 145, 165), rxl);
    }
    y = (int16_t)(y + 4);
    y = ui_draw_wrapped_gfx(t, &FreeSansBold12pt7b, kLayoutInset, y, y_hdr_max, max_chars12, lh_hdr,
                            ui_rgb565(160, 185, 210), "Assembling (incomplete line):");
    char partial[200];
    center_gps_diag_copy_partial(partial, sizeof(partial));
    y = ui_draw_wrapped_gfx(t, &FreeSansBold12pt7b, kLayoutInset, y, y_hdr_max, max_chars12, lh_hdr,
                            ui_rgb565(225, 220, 195), partial);

    y = (int16_t)(y + 6);
    double la = 0.0, lo = 0.0;
    const bool have_ll = center_gps_last_fix_coords(&la, &lo);
    char sum[120];
    if (have_ll) {
      snprintf(sum, sizeof(sum), "Fix pos=1  lat=%.6f  lon=%.6f", la, lo);
    } else {
      snprintf(sum, sizeof(sum), "Fix pos=0 (need RMC A or GGA q 1-9)");
    }
    y = ui_draw_wrapped_gfx(t, &FreeSansBold12pt7b, kLayoutInset, y, y_hdr_max, max_chars12, lh_hdr,
                            ui_rgb565(135, 200, 135), sum);

    const char *st = center_gps_street_line();
    char stline[100];
    if (st && st[0]) {
      snprintf(stline, sizeof(stline), "Street: %s", st);
    } else {
      strncpy(stline, "Street: (none yet)", sizeof(stline) - 1u);
      stline[sizeof(stline) - 1u] = '\0';
    }
    y = ui_draw_wrapped_gfx(t, &FreeSansBold12pt7b, kLayoutInset, y, y_hdr_max, max_chars12, lh_hdr,
                            ui_rgb565(175, 175, 220), stline);

    t->setFont(&FreeSansBold12pt7b);
    t->setTextSize(1);
    t->setTextColor(ui_rgb565(115, 140, 155));
    t->setCursor(kLayoutInset, y_list_top);
    t->print("Recent $ sentences (newest at bottom):");

    char rows[CENTER_GPS_DIAG_RECENT_LINES][128];
    center_gps_diag_copy_recent(rows);
    for (int i = 0; i < n_recent; i++) {
      const int16_t yy = (int16_t)(y_list_top + lh_stream + i * lh_stream);
      const uint16_t col = (i == n_recent - 1) ? ui_rgb565(255, 225, 120) : ui_rgb565(175, 180, 190);
      ui_print_trunc_gfx(t, kLayoutInset, yy, max_chars12, col, &FreeSansBold12pt7b, rows[i][0] ? rows[i] : "—");
    }

    t->setFont(&FreeSansBold10pt7b);
    t->setTextSize(1);
    t->setTextColor(0xFFFFu);
    const int16_t bw = 280;
    const int16_t bx = (int16_t)((sw - bw) / 2);
    t->fillRoundRect(bx, by_btn, bw, bh, 10, ui_rgb565(48, 48, 48));
    t->drawRoundRect(bx, by_btn, bw, bh, 10, ui_rgb565(140, 140, 140));
    int16_t bx0 = 0, by0 = 0;
    uint16_t tw = 0, th = 0;
    t->getTextBounds("BACK", 0, 0, &bx0, &by0, &tw, &th);
    t->setCursor((int16_t)(bx + (bw - (int16_t)tw) / 2), (int16_t)(by_btn + (bh + (int16_t)th) / 2));
    t->print("BACK");
  }

  t->setFont(nullptr);
  blit_round_canvas(gfx);
}

void center_ui_loop() {
  const uint32_t now = millis();
  center_menu_loop();
  ElcCanSettingRequest req;
  while (center_can_take_setting(&req)) {
    center_ui_apply_setting(req);
  }
  static uint32_t s_status_ms = 0;
  if (now - s_status_ms >= 1000u) {
    s_status_ms = now;
    const bool wf = WiFi.status() == WL_CONNECTED;
    ElcCanCenterStatus st;
    st.flags = (uint8_t)((wf ? ELC_CAN_STATUS_WIFI : 0) | (center_gps_have_position_fix() ? ELC_CAN_STATUS_GPS_FIX : 0) |
                         (center_gps_last_uart_byte_ms() && now - center_gps_last_uart_byte_ms() < 3000 ? ELC_CAN_STATUS_GPS_NMEA : 0) |
                         (center_audio_link_ok() ? ELC_CAN_STATUS_AUDIO : 0) |
                         (center_sensor_link().demo() ? ELC_CAN_STATUS_DEMO : 0) |
                         (s_prefs && s_prefs->getString("wifi_ssid", "").length() ? ELC_CAN_STATUS_SAVED_WIFI : 0) |
                         (center_menu_wifi_joining() ? ELC_CAN_STATUS_JOINING : 0));
    st.rssi = wf ? (int8_t)WiFi.RSSI() : 0;
    // The name: the connected network, else the saved one.
    const String name = wf ? WiFi.SSID() : (s_prefs ? s_prefs->getString("wifi_ssid", "") : String());
    center_can_send_status(st, name.c_str());
  }
  /* Rolling 2 s window: reset even when log is closed so the next open shows a fresh rate. */
  if (now - s_last_sensor_stats_ms >= 2000u) {
    s_last_sensor_stats_ms = now;
    const uint32_t pkts = s_sensor_pkts_since_stats;
    s_sensor_pkts_since_stats = 0;
    if (s_page == UiPage::FsSerial) {
      char b[kSensorLogChars];
      const wl_status_t wst = WiFi.status();
      const char *ws = (wst == WL_CONNECTED) ? "wifi=ok" : ((wst == WL_DISCONNECTED) ? "wifi=off" : "wifi=?");
      uint64_t wall_ms = 0;
      const bool wall = center_wall_epoch_ms(&wall_ms);
      const char *street = center_gps_street_line();
      char place[72];
      place[0] = '\0';
      if (street && street[0]) {
        strncpy(place, street, sizeof(place) - 1);
        place[sizeof(place) - 1] = '\0';
        /* One line: trim long names for the log width. */
        if (strlen(place) > 32u) {
          place[29] = '.';
          place[30] = '.';
          place[31] = '.';
          place[32] = '\0';
        }
      } else {
        strncpy(place, "(no street yet)", sizeof(place) - 1);
        place[sizeof(place) - 1] = '\0';
      }
      /* gbt = millis() when last byte arrived; (now - gbt) small/zero means UART is actively receiving. */
      const uint32_t gbt = center_gps_last_uart_byte_ms();
      char grx[32];
      if (gbt == 0) {
        strncpy(grx, "gps_rx=none", sizeof(grx) - 1);
        grx[sizeof(grx) - 1] = '\0';
      } else {
        snprintf(grx, sizeof(grx), "gps_rx_age=%lums", (unsigned long)(now - gbt));
      }
      const char *pos = center_gps_have_position_fix() ? "pos=1" : "pos=0";
      if (s_last_sensor_pkt_ms == 0) {
        snprintf(b, sizeof(b), "[status] %s %s %s wall=%s pkts/2s=%lu st=%s (no frames yet)", ws, grx, pos,
                 wall ? "set" : "unset", (unsigned long)pkts, place);
      } else {
        const uint32_t age = now - s_last_sensor_pkt_ms;
        snprintf(b, sizeof(b), "[status] %s %s %s wall=%s last_frame=%lums pkts/2s=%lu st=%s", ws, grx, pos,
                 wall ? "set" : "unset", (unsigned long)age, (unsigned long)pkts, place);
      }
      sensor_log_push_line(b);
    }
  }

  if (s_page == UiPage::FsWifiConn) {
    const int r = center_wifi_join_async_poll();
    if (r == 1) {
      center_ble_set_info_line("WiFi OK");
      close_to_gauge();
    } else if (r == 2) {
      menu_open_home(CenterMenuPage::Wifi);
    }
  }
}

void center_ui_on_touch_release(int16_t x, int16_t y) {
  if (!s_gfx || !s_prefs) {
    return;
  }
  if (!touch_in_circle(x, y)) {
    return;
  }

  if (center_ui_fullscreen()) {
    const int16_t sw = s_gfx->width();
    const int16_t sh = s_gfx->height();
    if (s_page == UiPage::FsPhone) {
      if (hit_cancel_bar(x, y, sw, sh)) {
        menu_open_home();
      }
      return;
    }
    if (s_page == UiPage::FsWifiScan || s_page == UiPage::FsSerial || s_page == UiPage::FsGpsDiag ||
        s_page == UiPage::FsWifiConn) {
      if (hit_cancel_bar(x, y, sw, sh)) {
        center_shell_set_wifi_psk_pending(nullptr);
        if (s_page == UiPage::FsWifiConn) {
          WiFi.disconnect(true, true);
        }
        menu_open_home();
      }
      return;
    }
    if (s_page == UiPage::FsPsk) {
      const int16_t h = hit_key(x, y);
      if (h == KK_CANCEL) {
        s_pwd_len = 0;
        s_pwd[0] = '\0';
        menu_open_home(CenterMenuPage::Wifi);
        return;
      }
      if (h == KK_SHIFT) {
        s_kb_shift = !s_kb_shift;
        return;
      }
      if (h == KK_BS) {
        if (s_pwd_len > 0) {
          s_pwd[--s_pwd_len] = '\0';
        }
        return;
      }
      if (h == KK_SPACE) {
        if (s_pwd_len < 63) {
          s_pwd[s_pwd_len++] = ' ';
          s_pwd[s_pwd_len] = '\0';
        }
        return;
      }
      if (h == KK_JOIN) {
        s_page = UiPage::FsWifiConn;
        center_wifi_join_async_start(*s_prefs, s_pending_ssid, s_pwd);
        s_pwd_len = 0;
        s_pwd[0] = '\0';
        s_kb_shift = false;
        return;
      }
      if (h > 32 && h < 127 && s_pwd_len < 63) {
        char c = (char)h;
        if (s_kb_shift && c >= 'a' && c <= 'z') {
          c = (char)toupper((unsigned char)c);
        }
        s_pwd[s_pwd_len++] = c;
        s_pwd[s_pwd_len] = '\0';
      }
      return;
    }
    return;
  }

  if (s_iface == ELC_CAN_SCREEN_CENTER) {
    center_menu_open(CenterMenuPage::Home);  // the menu is on another screen otherwise
  }
}
