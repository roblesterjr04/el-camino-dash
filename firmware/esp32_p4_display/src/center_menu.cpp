#include "center_menu.h"

#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

#include <cluster_i2c.h>
#include <display_setup.h>
#include <elc_can_ids.h>
#include <menu_can.h>
#include <sensor_link.h>
#include <ui_menu.h>

#include "center_audio_link.h"
#include "center_camera.h"
#include "center_can.h"
#include "center_gps.h"
#include "center_ui.h"
#include "center_wifi_ntp.h"

namespace {

Preferences *s_prefs = nullptr;
uint8_t s_level = 100;
bool s_follow = true;

bool s_joining = false;
char s_join_ssid[33];

// ---- settings
uint8_t h_theme() { return (uint8_t)center_ui_cluster_theme(); }
void h_set_theme(uint8_t t) { center_ui_set_theme(cluster_theme_from_reg(t)); }
uint8_t h_center_view() { return (uint8_t)center_ui_view_mode(); }
void h_set_center_view(uint8_t v) { center_ui_set_view((CenterViewMode)(v > 3 ? 0 : v)); }
bool h_dim() { return s_follow; }

// ---- Wi-Fi (the radio is here)
void h_wifi_status(char *l1, size_t n1, char *l2, size_t n2) {
  if (s_joining) {
    snprintf(l1, n1, "Connecting to %s\xE2\x80\xA6", s_join_ssid);
    snprintf(l2, n2, "%s", "");
  } else if (WiFi.status() == WL_CONNECTED) {
    snprintf(l1, n1, "Connected \xC2\xB7 %s", WiFi.SSID().c_str());
    snprintf(l2, n2, "%s   %d dBm", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  } else {
    snprintf(l1, n1, "Not connected");
    const String saved = s_prefs ? s_prefs->getString("wifi_ssid", "") : String();
    if (saved.length()) {
      snprintf(l2, n2, "Saved network: %s", saved.c_str());
    } else {
      snprintf(l2, n2, "No saved network");
    }
  }
}

bool h_wifi_has_saved() { return s_prefs && s_prefs->getString("wifi_ssid", "").length() > 0; }

void h_wifi_join_saved() {
  if (!h_wifi_has_saved()) return;
  snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", s_prefs->getString("wifi_ssid", "").c_str());
  s_joining = true;
  center_wifi_join_saved_async_start(*s_prefs);
}

void h_wifi_scan_start() { center_wifi_scan_async_start(); }

int h_wifi_scan_poll(UiMenuWifiNet *nets, int max, int *n) {
  char ssid[8][33];
  uint8_t enc[8];
  int k = 0;
  const int r = center_wifi_scan_async_poll(ssid, enc, max < 8 ? max : 8, &k);
  if (r == 1) {
    for (int i = 0; i < k; i++) {
      snprintf(nets[i].ssid, sizeof(nets[i].ssid), "%s", ssid[i]);
      nets[i].open = enc[i] == (uint8_t)WIFI_AUTH_OPEN;
    }
    *n = k;
  }
  return r;
}

void h_wifi_join(const char *ssid, bool open_net) {
  if (!open_net) {
    center_menu_close();
    center_ui_open_wifi_password(ssid);  // on-screen keyboard (legacy page)
    return;
  }
  snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", ssid);
  s_joining = true;
  center_wifi_join_async_start(*s_prefs, s_join_ssid, "");
}

void h_wifi_poll() {
  if (s_joining && center_wifi_join_async_poll() != 0) {
    s_joining = false;
  }
}

// ---- Service
void h_service_row(int row, char *b, size_t n, int *level) {
  const SensorLink &L = center_sensor_link();
  const bool fr = L.fresh();
  switch (row) {
    case 0:
      if (fr) {
        snprintf(b, n, "%s%lu frames/s", L.demo() ? "Demo \xC2\xB7 " : "", (unsigned long)L.framesPerSec());
      } else {
        snprintf(b, n, "No data");
      }
      *level = fr ? 0 : 2;
      break;
    case 1: {
      const bool odo = fr && L.last().has_odometer;
      if (odo) {
        snprintf(b, n, "%.1f mi", (double)L.last().odometer_tenths_mile * 0.1);
      } else {
        snprintf(b, n, "%s", fr ? "Unknown (FRAM)" : "\xE2\x80\x94");
      }
      *level = odo ? 0 : (fr ? 2 : 1);
      break;
    }
    case 2: {
      // Fault word bits from sensor_pack.ino (docs/protocols/sensor_uart.md).
      static const char *const kFault[16] = {"Fuel open", "Fuel short", "Temp open", "Temp short", "Oil open",
                                             "Oil short", "Volt open", "Volt high", "No speed", "No coil",
                                             "Alt stuck", "Fuel pwr", "Temp pwr", "Oil pwr", "Volt pwr", "Odo store"};
      const uint16_t f = fr ? L.last().faults : 0;
      b[0] = '\0';
      for (int bit = 0; bit < 16; bit++) {
        if (f & (1u << bit)) {
          if (b[0]) strncat(b, ", ", n - strlen(b) - 1);
          strncat(b, kFault[bit], n - strlen(b) - 1);
        }
      }
      if (!f) snprintf(b, n, "None");
      *level = f ? ((f & (1u << 15)) ? 2 : 1) : 0;
      break;
    }
    case 3: {
      const bool fix = center_gps_have_position_fix();
      const uint32_t gb = center_gps_last_uart_byte_ms();
      snprintf(b, n, "%s", fix ? "Fix" : (gb && millis() - gb < 3000 ? "No fix" : "No NMEA"));
      *level = fix ? 0 : (gb ? 1 : 2);
      break;
    }
    case 4: {
      const bool wf = WiFi.status() == WL_CONNECTED;
      snprintf(b, n, "%s", wf ? WiFi.SSID().c_str() : "Not connected");
      *level = wf ? 0 : 1;
      break;
    }
    case 5: {
      const bool au = center_audio_link_ok();
      snprintf(b, n, "%s", au ? "Connected" : "No response");
      *level = au ? 0 : 1;
      break;
    }
    default: {
      // The other screens: CAN heartbeat in the car, I2C ACK on a bench without transceivers.
      const bool left = row == 6;
      const bool can = center_can_node_alive(left ? ELC_CAN_NODE_SPEEDO : ELC_CAN_NODE_TSPS);
      const bool i2c = cluster_i2c_last_ack(left ? CLUSTER_I2C_ADDR_SPEEDO : CLUSTER_I2C_ADDR_TSPS);
      snprintf(b, n, "%s", can ? "On CAN" : (i2c ? "I2C ACK" : "No reply"));
      *level = (can || i2c) ? 0 : 1;
      break;
    }
  }
}

bool h_demo() { return center_sensor_link().demo(); }

// ---- the console (menu_can)
bool m_is_interface() { return center_ui_interface_screen() == ELC_CAN_SCREEN_CENTER; }
bool m_may_open() { return !center_camera_showing(); }

UiMenuHost make_host() {
  UiMenuHost h;
  h.self_screen = kUiScreenCenter;
  h.theme = h_theme;
  h.set_theme = h_set_theme;
  h.center_view = h_center_view;
  h.set_center_view = h_set_center_view;
  h.interface_screen = center_ui_interface_screen;
  h.set_interface_screen = center_ui_set_interface_screen;
  h.media_screen = center_ui_media_screen;
  h.set_media_screen = center_ui_set_media_screen;
  h.media_shown = center_ui_media_shown;
  h.show_media = center_ui_show_media;
  h.brightness = center_menu_brightness;
  h.dim_with_lights = h_dim;
  h.set_brightness = center_menu_set_brightness;
  h.local_wifi = true;
  h.wifi_status = h_wifi_status;
  h.wifi_has_saved = h_wifi_has_saved;
  h.wifi_join_saved = h_wifi_join_saved;
  h.wifi_scan_start = h_wifi_scan_start;
  h.wifi_scan_poll = h_wifi_scan_poll;
  h.wifi_join = h_wifi_join;
  h.wifi_poll = nullptr;  // center_menu_loop() polls joins, menu open or not
  h.service_row = h_service_row;
  h.demo = h_demo;
  h.set_demo = center_set_demo;
  h.open_center_page = center_menu_open_center_page;
  return h;
}

UiMenuHost s_host;

}  // namespace

void center_menu_begin(Preferences *prefs) {
  s_prefs = prefs;
  if (prefs) {
    s_level = prefs->getUChar("bright", 100);
    s_follow = prefs->getBool("bfollow", true);
  }
  if (s_level < 10 || s_level > 100) s_level = 100;
  elc_backlight_set_user(s_level, s_follow);
  s_host = make_host();
  ui_menu_begin(&s_host);
  MenuCanConfig mc;
  mc.self_screen = ELC_CAN_SCREEN_CENTER;
  mc.send = center_can_send;
  mc.is_interface = m_is_interface;
  mc.may_open = m_may_open;
  mc.before_open = center_ui_leave_fullscreen;
  menu_can_begin(mc);
}

uint8_t center_menu_brightness() { return s_level; }
bool center_menu_dim_with_lights() { return s_follow; }

void center_menu_set_brightness(uint8_t level, bool follow_lights, bool save) {
  s_level = level < 10 ? 10 : (level > 100 ? 100 : level);
  s_follow = follow_lights;
  elc_backlight_set_user(s_level, s_follow);
  if (save && s_prefs) {
    s_prefs->putUChar("bright", s_level);
    s_prefs->putBool("bfollow", s_follow);
    center_ui_sync_cluster_bus();  // the other screens follow the center's brightness
  }
}

void center_menu_open_center_page(uint8_t which) {
  switch (which) {
    case 1: center_menu_open(CenterMenuPage::Wifi); break;
    case 2: center_ui_open_legacy_page(CenterLegacyPage::SensorLog); break;
    case 3: center_ui_open_legacy_page(CenterLegacyPage::GpsDiag); break;
    case 4:  // older side screens asked for the legacy phone page: the menu's Phone page replaces it
      center_ui_leave_fullscreen();
      center_menu_open(CenterMenuPage::Phone);
      break;
    default: break;
  }
}

void center_menu_open(CenterMenuPage page) { ui_menu_open((UiMenuPage)(uint8_t)page); }

void center_menu_close() { ui_menu_close(); }

void center_menu_loop() {
  h_wifi_poll();  // a join started here or from another screen finishes (and is saved)
  menu_can_poll(millis());
}

bool center_menu_wifi_joining() { return s_joining; }

void center_menu_wifi_join_saved() { h_wifi_join_saved(); }

bool center_menu_is_open() { return ui_menu_is_open(); }

void center_menu_rotate(int steps) { ui_menu_rotate(steps); }

void center_menu_press() { ui_menu_press(); }

void center_menu_hold_feedback(uint32_t held_ms, bool active) {
  if (!active || held_ms < 350) {
    ui_menu_hold_feedback(false, 0, nullptr);
    return;
  }
  const ClusterTheme nt = center_ui_next_theme(center_ui_cluster_theme());
  const char *next = nt == ClusterTheme::Classic ? "Classic" : (nt == ClusterTheme::Modern ? "Modern" : "Fallout");
  static char buf[64];
  if (held_ms < 3000) {
    ui_menu_hold_feedback(true, (int32_t)(held_ms * 1000 / 3000), "Keep holding: next view");
  } else if (held_ms < 10000) {
    snprintf(buf, sizeof(buf), "Release: next view \xC2\xB7 hold: %s", next);
    ui_menu_hold_feedback(true, (int32_t)((held_ms - 3000) * 1000 / 7000), buf);
  } else {
    snprintf(buf, sizeof(buf), "Release: switch to %s", next);
    ui_menu_hold_feedback(true, 1000, buf);
  }
}
