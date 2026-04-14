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

#include <p4_round_mask.h>

#include "FreeSansBold10pt7b.h"
#include "center_audio_link.h"
#include "center_ble.h"
#include "center_i2c_host.h"
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
  More,
  FsPsk,
  FsI2c,
  FsBle,
  FsTz,
  FsWifiScan,
  FsWifiConn,
};

enum : int {
  CID_TACH = 0,
  CID_CLOCK = 1,
  CID_WIFI = 2,
  CID_MORE = 3,
  CID_W_USE = 10,
  CID_W_SCAN = 11,
  CID_W_BACK = 12,
  CID_W_TZ = 13,
  CID_M_I2C = 20,
  CID_M_BLE = 21,
  CID_M_BACK = 22,
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

static uint8_t s_i2c_addrs[24];
static int s_i2c_n = 0;

static Arduino_Canvas *s_fs_canvas = nullptr;

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

// UTC−12 … UTC+14 grid (whole hours). POSIX via Espressif-style GMT±N (see center_wifi_set_tz_utc_offset_hours).
static constexpr int kTzGridMinH = -12;
static constexpr int kTzGridMaxH = 14;
static constexpr int kTzGridCols = 6;

struct TzGridGeom {
  int16_t y0;
  int16_t cellw, cellh, gap;
};

static void tz_grid_geom(int16_t sw, TzGridGeom *g) {
  g->gap = 5;
  const int16_t inner = (int16_t)(sw - 2 * kLayoutInset);
  g->cellw = (int16_t)((inner - (kTzGridCols - 1) * g->gap) / kTzGridCols);
  g->cellh = 34;
  g->y0 = (int16_t)(kLayoutInset + 108);
}

static bool hit_tz_offset_grid(int16_t tx, int16_t ty, int16_t sw, int16_t /*sh*/, int *out_h) {
  TzGridGeom geom{};
  tz_grid_geom(sw, &geom);
  const int n = kTzGridMaxH - kTzGridMinH + 1;
  for (int idx = 0; idx < n; idx++) {
    const int row = idx / kTzGridCols;
    const int col = idx % kTzGridCols;
    const int16_t bx = (int16_t)(kLayoutInset + col * (geom.cellw + geom.gap));
    const int16_t by = (int16_t)(geom.y0 + row * (geom.cellh + geom.gap));
    if (tx >= bx && tx < bx + geom.cellw && ty >= by && ty < by + geom.cellh) {
      if (out_h) {
        *out_h = kTzGridMinH + idx;
      }
      return true;
    }
  }
  return false;
}

static void menu_open_home() {
  s_page = UiPage::Home;
  s_menu->setScreenSize(s_gfx->width(), s_gfx->height());
  s_menu->setBackdropColor(0x0000);
  s_menu->setupGrid2x2(CID_TACH, CID_CLOCK, CID_WIFI, CID_MORE, "TACH", "CLOCK", "WIFI", "MORE", kLayoutInset, 14);
  s_menu->open();
}

static void menu_wifi_tools() {
  s_page = UiPage::WifiTools;
  const int16_t sw = s_gfx->width();
  const int16_t sh = s_gfx->height();
  const int16_t m = kLayoutInset;
  const int16_t gap = 10;
  const int16_t h = (int16_t)((sh - 2 * m - 3 * gap) / 4);
  s_menu->clearItems();
  s_menu->addItem({CID_W_USE, m, m, (int16_t)(sw - 2 * m), h, "SAVED"});
  s_menu->addItem({CID_W_SCAN, m, (int16_t)(m + h + gap), (int16_t)(sw - 2 * m), h, "SCAN"});
  s_menu->addItem({CID_W_TZ, m, (int16_t)(m + 2 * (h + gap)), (int16_t)(sw - 2 * m), h, "TIME ZONE"});
  s_menu->addItem({CID_W_BACK, m, (int16_t)(m + 3 * (h + gap)), (int16_t)(sw - 2 * m), h, "BACK"});
  s_menu->open();
}

static void menu_more() {
  s_page = UiPage::More;
  const int16_t sw = s_gfx->width();
  const int16_t sh = s_gfx->height();
  const int16_t m = kLayoutInset;
  const int16_t gap = 10;
  const int16_t h = (int16_t)((sh - 2 * m - 2 * gap) / 3);
  s_menu->clearItems();
  s_menu->addItem({CID_M_I2C, m, m, (int16_t)(sw - 2 * m), h, "I2C BUS"});
  s_menu->addItem({CID_M_BLE, m, (int16_t)(m + h + gap), (int16_t)(sw - 2 * m), h, "BT INFO"});
  s_menu->addItem({CID_M_BACK, m, (int16_t)(m + 2 * (h + gap)), (int16_t)(sw - 2 * m), h, "BACK"});
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

void center_ui_begin(Arduino_GFX *gfx, Preferences *prefs, P4OverlayMenu *menu) {
  s_gfx = gfx;
  s_prefs = prefs;
  s_menu = menu;
  if (prefs) {
    s_theme = (prefs->getUChar("ctheme", 0) != 0) ? ClusterTheme::Fallout : ClusterTheme::Classic;
    uint8_t v = prefs->getUChar("view", 0);
    if (v > 2) {
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
    v = (uint8_t)((v + 1u) % 3u);
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
  return s_page == UiPage::FsPsk || s_page == UiPage::FsI2c || s_page == UiPage::FsBle || s_page == UiPage::FsTz ||
         s_page == UiPage::FsWifiScan || s_page == UiPage::FsWifiConn;
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
  const int hi = (s_view == CenterViewMode::Tach) ? CID_TACH : (s_view == CenterViewMode::Clock) ? CID_CLOCK : -999;
  if (s_page == UiPage::Home) {
    s_menu->draw((int16_t)hi);
  } else {
    s_menu->draw(-999);
  }
  if (s_page == UiPage::WifiTools && s_gfx) {
    draw_wifi_tools_status(s_gfx);
  }
}

static void run_i2c_scan_page() {
  center_i2c_host_scan(s_i2c_addrs, 24, &s_i2c_n);
  s_page = UiPage::FsI2c;
  s_menu->close();
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
  } else if (s_page == UiPage::FsI2c) {
    t->setCursor(kLayoutInset, kLayoutInset);
    t->print("I2C (host scan)");
    int y = kLayoutInset + 36;
    for (int i = 0; i < s_i2c_n; i++) {
      char line[24];
      snprintf(line, sizeof(line), "0x%02X", (unsigned)s_i2c_addrs[i]);
      t->setCursor(kLayoutInset, (int16_t)y);
      t->print(line);
      y += 26;
      if (y > sh - 100) {
        break;
      }
    }
    if (s_i2c_n == 0) {
      t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 40));
      t->print("(none)");
    }
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
  } else if (s_page == UiPage::FsTz) {
    t->setCursor(kLayoutInset, kLayoutInset);
    t->print("Time zone");
    t->setTextColor(ui_rgb565(160, 160, 160));
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 24));
    t->print("NTP = UTC; pick fixed offset (no DST).");
    String cur = s_prefs ? s_prefs->getString("tz", CENTER_TZ_DEFAULT) : String(CENTER_TZ_DEFAULT);
    t->setTextColor(0xFFFFu);
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 48));
    t->print("Now: ");
    if (cur.length() > 28) {
      t->print(cur.substring(0, 25));
      t->print("...");
    } else {
      t->print(cur);
    }
    {
      TzGridGeom geom{};
      tz_grid_geom(sw, &geom);
      const int n = kTzGridMaxH - kTzGridMinH + 1;
      for (int idx = 0; idx < n; idx++) {
        const int h = kTzGridMinH + idx;
        const int row = idx / kTzGridCols;
        const int col = idx % kTzGridCols;
        const int16_t bx = (int16_t)(kLayoutInset + col * (geom.cellw + geom.gap));
        const int16_t by = (int16_t)(geom.y0 + row * (geom.cellh + geom.gap));
        char lbl[12];
        if (h == 0) {
          strncpy(lbl, "UTC", sizeof(lbl) - 1);
          lbl[sizeof(lbl) - 1] = '\0';
        } else {
          snprintf(lbl, sizeof(lbl), "%+d", h);
        }
        t->fillRoundRect(bx, by, geom.cellw, geom.cellh, 6, ui_rgb565(38, 38, 44));
        t->drawRoundRect(bx, by, geom.cellw, geom.cellh, 6, ui_rgb565(110, 110, 130));
        int16_t bx0 = 0, by0 = 0;
        uint16_t tw = 0, th = 0;
        t->getTextBounds(lbl, 0, 0, &bx0, &by0, &tw, &th);
        t->setTextColor(0xFFFFu);
        t->setCursor((int16_t)(bx + (geom.cellw - (int16_t)tw) / 2), (int16_t)(by + (geom.cellh + (int16_t)th) / 2));
        t->print(lbl);
      }
    }
    const int16_t cbw = 280;
    const int16_t cbh = 48;
    const int16_t cbx = (int16_t)((sw - cbw) / 2);
    const int16_t cby = (int16_t)(sh - kLayoutInset - 8 - cbh);
    t->fillRoundRect(cbx, cby, cbw, cbh, 10, ui_rgb565(48, 48, 48));
    t->drawRoundRect(cbx, cby, cbw, cbh, 10, ui_rgb565(140, 140, 140));
    int16_t bx0 = 0, by0 = 0;
    uint16_t tw = 0, th = 0;
    t->getTextBounds("BACK", 0, 0, &bx0, &by0, &tw, &th);
    t->setTextColor(0xFFFFu);
    t->setCursor((int16_t)(cbx + (cbw - (int16_t)tw) / 2), (int16_t)(cby + (cbh + (int16_t)th) / 2));
    t->print("BACK");
  } else if (s_page == UiPage::FsBle) {
    t->setCursor(kLayoutInset, kLayoutInset);
    t->print("Bluetooth on this display");
    t->setTextColor(ui_rgb565(160, 160, 160));
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 26));
    t->print("BLE: status / tooling only (no phone A2DP).");
    t->setTextColor(0xFFFFu);
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 52));
    if (center_ble_is_connected()) {
      t->setTextColor(ui_rgb565(120, 200, 120));
      t->print("BLE: connected");
    } else {
      t->setTextColor(ui_rgb565(220, 200, 120));
      t->print("BLE: advertising");
    }
    t->setTextColor(ui_rgb565(180, 220, 180));
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 78));
    t->print("ElCamino");
    t->setTextColor(ui_rgb565(140, 140, 160));
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 104));
    t->print("(scanner apps, not phone Settings)");
    t->setTextColor(ui_rgb565(160, 160, 160));
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 134));
    t->print("Classic BT audio: external ESP32 (A2DP)");
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 160));
    if (center_audio_link_ok()) {
      t->setTextColor(ui_rgb565(120, 200, 120));
      t->print("I2C metadata 0x2A: OK");
    } else {
      t->setTextColor(ui_rgb565(255, 170, 120));
      t->print("I2C metadata 0x2A: offline");
    }
    t->setTextColor(ui_rgb565(200, 200, 210));
    t->setCursor(kLayoutInset, (int16_t)(kLayoutInset + 188));
    t->print("Now:");
    t->setCursor((int16_t)(kLayoutInset + 52), (int16_t)(kLayoutInset + 188));
    {
      const char *np = center_audio_link_line();
      if (np && np[0]) {
        char buf[36];
        strncpy(buf, np, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        t->print(buf);
      } else {
        t->setTextColor(ui_rgb565(120, 120, 130));
        t->print("—");
      }
    }
    const int16_t bw = 280;
    const int16_t bh = 48;
    const int16_t bx = (int16_t)((sw - bw) / 2);
    const int16_t by = (int16_t)(sh - kLayoutInset - 8 - bh);
    t->setTextColor(0xFFFFu);
    t->fillRoundRect(bx, by, bw, bh, 10, ui_rgb565(48, 48, 48));
    t->drawRoundRect(bx, by, bw, bh, 10, ui_rgb565(140, 140, 140));
    int16_t bx0 = 0, by0 = 0;
    uint16_t tw = 0, th = 0;
    t->getTextBounds("CANCEL", 0, 0, &bx0, &by0, &tw, &th);
    t->setCursor((int16_t)(bx + (bw - (int16_t)tw) / 2), (int16_t)(by + (bh + (int16_t)th) / 2));
    t->print("CANCEL");
  }

  t->setFont(nullptr);
  blit_round_canvas(gfx);
}

void center_ui_loop() {
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
    if (s_page == UiPage::FsTz) {
      if (hit_cancel_bar(x, y, sw, sh)) {
        menu_wifi_tools();
        return;
      }
      int uh = 0;
      if (hit_tz_offset_grid(x, y, sw, sh, &uh)) {
        center_wifi_set_tz_utc_offset_hours(*s_prefs, uh);
        menu_wifi_tools();
      }
      return;
    }
    if (s_page == UiPage::FsWifiScan || s_page == UiPage::FsI2c || s_page == UiPage::FsBle || s_page == UiPage::FsWifiConn) {
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

  const int pick = s_menu->onRelease(x, y);
  if (pick == -2) {
    close_to_gauge();
    return;
  }
  if (pick == -3) {
    return;
  }

  if (s_page == UiPage::Home) {
    if (pick == CID_TACH) {
      s_view = CenterViewMode::Tach;
      s_prefs->putUChar("view", 0);
      center_sd_settings_sync(*s_prefs);
      center_ui_sync_cluster_bus();
      close_to_gauge();
    } else if (pick == CID_CLOCK) {
      s_view = CenterViewMode::Clock;
      s_prefs->putUChar("view", 1);
      center_sd_settings_sync(*s_prefs);
      center_ui_sync_cluster_bus();
      close_to_gauge();
    } else if (pick == CID_WIFI) {
      menu_wifi_tools();
    } else if (pick == CID_MORE) {
      menu_more();
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
    } else if (pick == CID_W_TZ) {
      s_page = UiPage::FsTz;
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
        if (center_wifi_join_and_save(*s_prefs, s_pending_ssid, "")) {
          center_ble_set_info_line("WiFi OK");
        }
        close_to_gauge();
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

  if (s_page == UiPage::More) {
    if (pick == CID_M_BACK) {
      menu_open_home();
    } else if (pick == CID_M_I2C) {
      run_i2c_scan_page();
    } else if (pick == CID_M_BLE) {
      s_page = UiPage::FsBle;
      s_menu->close();
    }
  }
}
