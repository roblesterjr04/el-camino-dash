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
#include <sensor_packet.h>

#include <p4_round_mask.h>

#include "FreeSansBold10pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "center_ble.h"
#include "center_gps.h"
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
  WifiTools,
  WifiList,
  FsPhone,
  FsPsk,
  FsSerial,
  FsGpsDiag,
  FsWifiScan,
  FsWifiConn,
};

enum : int {
  CID_H_CLOCK = 1,
  CID_H_TACH = 2,
  CID_H_BT = 3,
  CID_H_PHONE = 4,
  CID_H_THEME = 5,
  CID_H_WIFI = 6,
  CID_H_ECON = 7,
  CID_W_USE = 10,
  CID_W_SCAN = 11,
  CID_W_BACK = 12,
  CID_W_SERIAL = 14,
  CID_W_GPS = 15,
  CID_SS_BASE = 100,
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
static P4OverlayMenu *s_menu = nullptr;
static UiPage s_page = UiPage::Closed;
static CenterViewMode s_view = CenterViewMode::Tach;
static ClusterTheme s_theme = ClusterTheme::Classic;

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

static int16_t s_touch_drag_accum = 0;
static int16_t s_touch_last_y = -1;
static bool s_touch_prev_down = false;

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

static void menu_open_home() {
  s_page = UiPage::Home;
  s_menu->setScreenSize(s_gfx->width(), s_gfx->height());
  s_menu->setBackdropColor(0x0000);
  static const int kHomeIds[] = {CID_H_CLOCK, CID_H_TACH, CID_H_ECON, CID_H_BT, CID_H_PHONE, CID_H_THEME, CID_H_WIFI};
  static const char *const kHomeLbl[] = {"Clock",      "Tachometer",      "Efficiency (MPG)",
                                         "Bluetooth Audio", "Phone", "Theme Toggle", "WiFi Settings"};
  s_menu->setupHomeTranslateList(kHomeIds, kHomeLbl, 7, kLayoutInset);
  s_menu->open();
}

static void menu_wifi_tools() {
  s_page = UiPage::WifiTools;
  const int16_t sw = s_gfx->width();
  const int16_t sh = s_gfx->height();
  const int16_t m = kLayoutInset;
  const int16_t gap = 8;
  const int rows = 5;
  const int16_t h = (int16_t)((sh - 2 * m - (rows - 1) * gap) / rows);
  s_menu->clearItems();
  s_menu->addItem({CID_W_USE, m, m, (int16_t)(sw - 2 * m), h, "SAVED"});
  s_menu->addItem({CID_W_SCAN, m, (int16_t)(m + 1 * (h + gap)), (int16_t)(sw - 2 * m), h, "SCAN"});
  s_menu->addItem({CID_W_SERIAL, m, (int16_t)(m + 2 * (h + gap)), (int16_t)(sw - 2 * m), h, "SENSOR LOG"});
  s_menu->addItem({CID_W_GPS, m, (int16_t)(m + 3 * (h + gap)), (int16_t)(sw - 2 * m), h, "GPS UART"});
  s_menu->addItem({CID_W_BACK, m, (int16_t)(m + 4 * (h + gap)), (int16_t)(sw - 2 * m), h, "BACK"});
  s_menu->open();
}

static void menu_wifi_list() {
  s_page = UiPage::WifiList;
  const int16_t sw = s_gfx->width();
  const int16_t sh = s_gfx->height();
  const int16_t m = kLayoutInset;
  const int16_t back_h = 48;
  const int16_t n = (int16_t)s_scan_n;
  if (n <= 0) {
    s_menu->clearItems();
    s_menu->addItem({CID_W_BACK, m, (int16_t)(sh - m - back_h), (int16_t)(sw - 2 * m), back_h, "BACK"});
    s_menu->open();
    return;
  }
  const int16_t avail = (int16_t)(sh - 2 * m - back_h - 8);
  const int16_t row = (int16_t)(avail / n);
  s_menu->clearItems();
  for (int i = 0; i < s_scan_n; i++) {
    const int16_t y = (int16_t)(m + i * row);
    s_menu->addItem({CID_SS_BASE + i, m, y, (int16_t)(sw - 2 * m), (int16_t)(row - 4), s_scan_ssid[i]});
  }
  s_menu->addItem({CID_W_BACK, m, (int16_t)(sh - m - back_h), (int16_t)(sw - 2 * m), back_h, "BACK"});
  s_menu->open();
}

static void close_to_gauge() {
  s_menu->close();
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

void center_ui_begin(Arduino_GFX *gfx, Preferences *prefs, P4OverlayMenu *menu) {
  s_gfx = gfx;
  s_prefs = prefs;
  s_menu = menu;
  if (prefs) {
    s_theme = (prefs->getUChar("ctheme", 0) != 0) ? ClusterTheme::Fallout : ClusterTheme::Classic;
    uint8_t v = prefs->getUChar("view", 0);
    if (v > 3) {
      v = 0;
    }
    s_view = (CenterViewMode)v;
  }
}

CenterViewMode center_ui_view_mode() { return s_view; }

ClusterTheme center_ui_cluster_theme() { return s_theme; }

void center_ui_sync_cluster_bus() {
  if (s_theme != ClusterTheme::Classic) {
    cluster_i2c_broadcast_center_theme(s_theme, ClusterCenterSubview::Tach);
    return;
  }
  cluster_i2c_broadcast_center_theme(s_theme, (ClusterCenterSubview)(uint8_t)s_view);
}

void center_ui_process_touch_gesture(uint32_t duration_ms, int16_t x, int16_t y) {
  if (!s_menu || !s_gfx || !s_prefs) {
    return;
  }
  if (!touch_in_circle(x, y)) {
    return;
  }
  if (center_ui_fullscreen() || center_ui_overlay_open()) {
    if (duration_ms < 3000u) {
      center_ui_on_touch_release(x, y);
    }
    return;
  }

  if (duration_ms >= 10000u) {
    s_theme = (s_theme == ClusterTheme::Classic) ? ClusterTheme::Fallout : ClusterTheme::Classic;
    s_prefs->putUChar("ctheme", (uint8_t)s_theme);
    center_sd_settings_sync(*s_prefs);
    center_ui_sync_cluster_bus();
    return;
  }
  if (duration_ms >= 3000u && s_theme == ClusterTheme::Classic) {
    uint8_t v = (uint8_t)s_view;
    v = (uint8_t)((v + 1u) % 4u);
    s_view = (CenterViewMode)v;
    s_prefs->putUChar("view", v);
    center_sd_settings_sync(*s_prefs);
    center_ui_sync_cluster_bus();
    return;
  }
  center_ui_on_touch_release(x, y);
}

bool center_ui_overlay_open() { return s_menu && s_menu->isOpen(); }

bool center_ui_fullscreen() {
  return s_page == UiPage::FsPsk || s_page == UiPage::FsSerial || s_page == UiPage::FsGpsDiag ||
         s_page == UiPage::FsWifiScan || s_page == UiPage::FsWifiConn || s_page == UiPage::FsPhone;
}

static void draw_wifi_tools_status(Arduino_GFX *g) {
  if (!g) {
    return;
  }
  const int16_t sw = g->width();
  const int16_t sh = g->height();
  (void)sh;
  char line[48];
  g->setFont(&FreeSansBold10pt7b);
  g->setTextSize(1);

  auto line_y = [&](int idx) -> int16_t { return (int16_t)(28 + idx * 22); };

  auto print_centered = [&](const char *s, int idx) {
    int16_t bx = 0, by = 0;
    uint16_t tw = 0, th = 0;
    g->getTextBounds(s, 0, 0, &bx, &by, &tw, &th);
    const int16_t x = (int16_t)((sw - (int16_t)tw) / 2);
    g->setCursor(x, line_y(idx));
    g->print(s);
  };

  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    g->setTextColor(ui_rgb565(140, 220, 140));
    print_centered("Wi-Fi connected", 0);

    String ss = WiFi.SSID();
    if (ss.length() > 30) {
      ss = ss.substring(0, 27) + "...";
    }
    g->setTextColor(0xFFFFu);
    print_centered(ss.c_str(), 1);

    snprintf(line, sizeof(line), "IP  %s", WiFi.localIP().toString().c_str());
    g->setTextColor(ui_rgb565(170, 200, 255));
    print_centered(line, 2);

    const int rssi = WiFi.RSSI();
    snprintf(line, sizeof(line), "%d dBm", rssi);
    g->setTextColor(ui_rgb565(160, 180, 160));
    print_centered(line, 3);
  } else {
    g->setTextColor(ui_rgb565(220, 180, 120));
    const char *msg = "Wi-Fi disconnected";
    if (st == WL_IDLE_STATUS) {
      msg = "Wi-Fi idle";
    } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
      msg = "Wi-Fi: network unavailable";
    } else if (st != WL_DISCONNECTED) {
      snprintf(line, sizeof(line), "Wi-Fi status %d", (int)st);
      msg = line;
    }
    print_centered(msg, 0);

    if (s_prefs) {
      String saved = s_prefs->getString("wifi_ssid", "");
      if (saved.length() > 0) {
        g->setTextColor(ui_rgb565(140, 140, 150));
        if (saved.length() > 32) {
          saved = saved.substring(0, 29) + "...";
        }
        snprintf(line, sizeof(line), "Saved: %s", saved.c_str());
        print_centered(line, 1);
      }
    }
  }
  g->setFont(nullptr);
}

void center_ui_draw_overlay() {
  if (!s_menu || !s_menu->isOpen()) {
    return;
  }
  int hi = -999;
  if (s_view == CenterViewMode::Tach) {
    hi = CID_H_TACH;
  } else if (s_view == CenterViewMode::Clock) {
    hi = CID_H_CLOCK;
  } else if (s_view == CenterViewMode::Efficiency) {
    hi = CID_H_ECON;
  } else if (s_view == CenterViewMode::Media) {
    hi = CID_H_BT;
  }
  if (s_page == UiPage::Home) {
    s_menu->draw((int16_t)hi);
  } else {
    s_menu->draw(-999);
  }
  if (s_page == UiPage::WifiTools && s_gfx) {
    draw_wifi_tools_status(s_gfx);
  }
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

void center_ui_touch_sample(int16_t x, int16_t y, bool finger_down) {
  (void)x;
  if (!s_menu || !s_menu->isOpen() || center_ui_fullscreen()) {
    if (!finger_down) {
      s_touch_prev_down = false;
      s_touch_last_y = -1;
    }
    return;
  }
  if (finger_down && !s_touch_prev_down) {
    s_touch_drag_accum = 0;
    s_touch_last_y = -1;
  }
  if (finger_down) {
    if (s_touch_last_y < 0) {
      s_touch_last_y = y;
      if (s_page == UiPage::Home && s_menu->isHomeTranslateList()) {
        s_menu->homeListBeginDrag(y);
      }
    } else {
      const int dy = (int)y - (int)s_touch_last_y;
      s_touch_drag_accum = (int16_t)(s_touch_drag_accum + (dy < 0 ? -dy : dy));
      s_touch_last_y = y;
      if (s_page == UiPage::Home && s_menu->isHomeTranslateList()) {
        s_menu->homeListDragTo(y);
      }
    }
  } else {
    s_touch_last_y = -1;
  }
  s_touch_prev_down = finger_down;
}

void center_ui_loop() {
  const uint32_t now = millis();
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

  if (s_page == UiPage::FsWifiScan) {
    int nout = 0;
    const int pr = center_wifi_scan_async_poll(s_scan_ssid, s_scan_enc, 8, &nout);
    if (pr != 0) {
      s_scan_n = nout;
      menu_wifi_list();
    }
  }
  if (s_page == UiPage::FsWifiConn) {
    const int r = center_wifi_join_async_poll();
    if (r == 1) {
      center_ble_set_info_line("WiFi OK");
      close_to_gauge();
    } else if (r == 2) {
      menu_wifi_tools();
    }
  }
}

static void start_fs_scan() {
  s_page = UiPage::FsWifiScan;
  s_menu->close();
  center_wifi_scan_async_start();
}

void center_ui_on_touch_release(int16_t x, int16_t y) {
  if (!s_menu || !s_gfx || !s_prefs) {
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
        center_shell_set_wifi_psk_pending(nullptr);
        s_pwd_len = 0;
        s_pwd[0] = '\0';
        s_kb_shift = false;
        menu_open_home();
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

  if (!s_menu->isOpen()) {
    menu_open_home();
    return;
  }

  const int16_t drag = s_touch_drag_accum;
  if (s_page == UiPage::Home && s_menu->isHomeTranslateList() && drag > 20) {
    s_menu->homeListSnap();
    s_touch_drag_accum = 0;
    return;
  }

  const int pick = s_menu->onRelease(x, y, drag);
  s_touch_drag_accum = 0;
  if (pick == -2) {
    close_to_gauge();
    return;
  }
  if (pick == -3) {
    return;
  }

  if (s_page == UiPage::Home) {
    if (pick == CID_H_TACH) {
      s_view = CenterViewMode::Tach;
      s_prefs->putUChar("view", 0);
      center_sd_settings_sync(*s_prefs);
      center_ui_sync_cluster_bus();
      close_to_gauge();
    } else if (pick == CID_H_CLOCK) {
      s_view = CenterViewMode::Clock;
      s_prefs->putUChar("view", 1);
      center_sd_settings_sync(*s_prefs);
      center_ui_sync_cluster_bus();
      close_to_gauge();
    } else if (pick == CID_H_ECON) {
      s_view = CenterViewMode::Efficiency;
      s_prefs->putUChar("view", 3);
      center_sd_settings_sync(*s_prefs);
      center_ui_sync_cluster_bus();
      close_to_gauge();
    } else if (pick == CID_H_BT) {
      s_view = CenterViewMode::Media;
      s_prefs->putUChar("view", 2);
      center_sd_settings_sync(*s_prefs);
      center_ui_sync_cluster_bus();
      close_to_gauge();
    } else if (pick == CID_H_PHONE) {
      s_page = UiPage::FsPhone;
      s_menu->close();
    } else if (pick == CID_H_THEME) {
      s_theme = (s_theme == ClusterTheme::Classic) ? ClusterTheme::Fallout : ClusterTheme::Classic;
      s_prefs->putUChar("ctheme", (uint8_t)s_theme);
      center_sd_settings_sync(*s_prefs);
      center_ui_sync_cluster_bus();
      close_to_gauge();
    } else if (pick == CID_H_WIFI) {
      menu_wifi_tools();
    }
    return;
  }

  if (s_page == UiPage::WifiTools) {
    if (pick == CID_W_BACK) {
      menu_open_home();
    } else if (pick == CID_W_USE) {
      s_page = UiPage::FsWifiConn;
      s_menu->close();
      center_wifi_join_saved_async_start(*s_prefs);
    } else if (pick == CID_W_SCAN) {
      start_fs_scan();
    } else if (pick == CID_W_SERIAL) {
      s_page = UiPage::FsSerial;
      s_menu->close();
    } else if (pick == CID_W_GPS) {
      s_page = UiPage::FsGpsDiag;
      s_menu->close();
    }
    return;
  }

  if (s_page == UiPage::WifiList) {
    if (pick == CID_W_BACK) {
      menu_wifi_tools();
    } else if (pick >= CID_SS_BASE && pick < CID_SS_BASE + s_scan_n) {
      const int idx = pick - CID_SS_BASE;
      strncpy(s_pending_ssid, s_scan_ssid[idx], 32);
      s_pending_ssid[32] = '\0';
      const uint8_t enc = s_scan_enc[idx];
      const bool is_open = (enc == (uint8_t)WIFI_AUTH_OPEN);
      if (is_open) {
        s_page = UiPage::FsWifiConn;  // center_ui_loop polls the async join and closes on success
        s_menu->close();
        center_wifi_join_async_start(*s_prefs, s_pending_ssid, "");
      } else {
        center_shell_set_wifi_psk_pending(s_pending_ssid);
        s_pwd_len = 0;
        s_pwd[0] = '\0';
        s_kb_shift = false;
        s_menu->close();
        s_page = UiPage::FsPsk;
        layout_wifi_keyboard(s_gfx->width(), s_gfx->height());
      }
    }
    return;
  }

}
