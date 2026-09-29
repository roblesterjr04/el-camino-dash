#include "cluster_peer.h"

#include <stdio.h>
#include <string.h>

#include <display_setup.h>
#include <media_can.h>
#include <p4_screen.h>
#include <sensor_link.h>
#include <ui_menu.h>

#if defined(ARDUINO)
#include <Arduino.h>
#else
extern "C" uint32_t millis(void);
#endif

namespace {

ClusterPeerConfig C;
ElcCanClusterState s_st;
bool s_have_st = false;
uint32_t s_st_ms = 0;

ElcCanCenterStatus s_status;
uint32_t s_status_ms = 0;
ElcCanTextDecoder s_name(ELC_CAN_ID_CENTER_TEXT);

// Brightness applied to this panel: the center's setting, or the level being dragged on this screen's menu.
uint8_t s_applied_level = 0;
bool s_applied_follow = false;

bool center_heard() { return s_status_ms && millis() - s_status_ms < 3000; }

void request(uint8_t setting, uint8_t value) {
  if (!C.send) {
    return;
  }
  ElcCanFrame f;
  elc_can_encode_setting_request(ElcCanSettingRequest{setting, value, C.node}, f);
  C.send(f);
}

// ---- UiMenuHost over CAN
uint8_t h_theme() { return s_st.theme; }
void h_set_theme(uint8_t t) { request(ELC_CAN_SET_THEME, t); }
uint8_t h_center_view() { return s_st.center_subview; }
void h_set_center_view(uint8_t v) { request(ELC_CAN_SET_CENTER_VIEW, v); }
uint8_t h_interface() { return s_st.interface_screen; }
void h_set_interface(uint8_t s) { request(ELC_CAN_SET_INTERFACE_SCREEN, s); }
uint8_t h_media() { return s_st.media_screen; }
void h_set_media(uint8_t s) { request(ELC_CAN_SET_MEDIA_SCREEN, s); }
bool h_media_shown() { return (s_st.flags & ELC_CAN_CLUSTER_MEDIA_SHOWN) != 0; }
void h_show_media(bool on) { request(ELC_CAN_SET_MEDIA_SHOWN, on ? 1 : 0); }
uint8_t h_brightness() { return s_st.brightness; }
bool h_dim() { return (s_st.flags & ELC_CAN_CLUSTER_DIM_WITH_LIGHTS) != 0; }

void h_set_brightness(uint8_t level, bool follow, bool save) {
  elc_backlight_set_user(level, follow);  // feel it here while dragging
  s_applied_level = level;
  s_applied_follow = follow;
  if (save) {
    if (level != s_st.brightness) request(ELC_CAN_SET_BRIGHTNESS, level);
    if (follow != h_dim()) request(ELC_CAN_SET_DIM_WITH_LIGHTS, follow ? 1 : 0);
  }
}

void h_wifi_status(char *l1, size_t n1, char *l2, size_t n2) {
  const char *name = s_name.valid() && s_name.serial() == s_status.text_serial ? s_name.text() : "";
  if (!center_heard()) {
    snprintf(l1, n1, "Center not heard");
    snprintf(l2, n2, "Wi-Fi is on the center screen");
  } else if (s_status.flags & ELC_CAN_STATUS_JOINING) {
    snprintf(l1, n1, "Connecting\xE2\x80\xA6");
    snprintf(l2, n2, "%s", name);
  } else if (s_status.flags & ELC_CAN_STATUS_WIFI) {
    snprintf(l1, n1, "Connected \xC2\xB7 %s", name);
    snprintf(l2, n2, "%d dBm", (int)s_status.rssi);
  } else {
    snprintf(l1, n1, "Not connected");
    if (s_status.flags & ELC_CAN_STATUS_SAVED_WIFI) {
      snprintf(l2, n2, "Saved network: %s", name);
    } else {
      snprintf(l2, n2, "No saved network");
    }
  }
}

bool h_wifi_has_saved() { return (s_status.flags & ELC_CAN_STATUS_SAVED_WIFI) != 0; }
void h_wifi_join_saved() { request(ELC_CAN_ACT_WIFI_JOIN_SAVED, 0); }

void h_service_row(int row, char *b, size_t n, int *level) {
  const bool center = center_heard();
  switch (row) {
    case 0:
    case 1:
    case 2: {
      const SensorLink *L = C.link;
      const bool fr = L && L->fresh();
      if (row == 0) {
        if (fr) {
          snprintf(b, n, "%s%lu frames/s", L->demo() ? "Demo \xC2\xB7 " : "", (unsigned long)L->framesPerSec());
        } else {
          snprintf(b, n, "No data");
        }
        *level = fr ? 0 : 2;
      } else if (row == 1) {
        const bool odo = fr && L->last().has_odometer;
        if (odo) {
          snprintf(b, n, "%.1f mi", (double)L->last().odometer_tenths_mile * 0.1);
        } else {
          snprintf(b, n, "%s", fr ? "Unknown (FRAM)" : "\xE2\x80\x94");
        }
        *level = odo ? 0 : (fr ? 2 : 1);
      } else {
        const uint16_t f = fr ? L->last().faults : 0;
        if (f) {
          snprintf(b, n, "0x%04X", (unsigned)f);  // names are on the center's Service page
        } else {
          snprintf(b, n, "None");
        }
        *level = f ? ((f & (1u << 15)) ? 2 : 1) : 0;
      }
      break;
    }
    case 3: {
      const bool fix = center && (s_status.flags & ELC_CAN_STATUS_GPS_FIX);
      const bool nmea = center && (s_status.flags & ELC_CAN_STATUS_GPS_NMEA);
      snprintf(b, n, "%s", !center ? "\xE2\x80\x94" : (fix ? "Fix" : (nmea ? "No fix" : "No NMEA")));
      *level = fix ? 0 : (nmea ? 1 : 2);
      break;
    }
    case 4: {
      const bool wf = center && (s_status.flags & ELC_CAN_STATUS_WIFI);
      char l2[40];
      h_wifi_status(b, n, l2, sizeof(l2));
      *level = wf ? 0 : 1;
      break;
    }
    case 5: {
      const bool au = media_can_now_playing(nullptr, 0, nullptr, nullptr, nullptr);
      snprintf(b, n, "%s", au ? "On CAN" : "Not heard");
      *level = au ? 0 : 1;
      break;
    }
    default: {
      const uint8_t screen = row == 6 ? ELC_CAN_SCREEN_LEFT : ELC_CAN_SCREEN_RIGHT;
      if (screen == C.self_screen) {
        snprintf(b, n, "This screen");
        *level = 0;
      } else {
        const bool alive = C.node_alive && C.node_alive(row == 6 ? ELC_CAN_NODE_SPEEDO : ELC_CAN_NODE_TSPS);
        snprintf(b, n, "%s", alive ? "On CAN" : "No reply");
        *level = alive ? 0 : 1;
      }
      break;
    }
  }
}

bool h_demo() { return (s_status.flags & ELC_CAN_STATUS_DEMO) != 0; }
void h_set_demo(bool on) { request(ELC_CAN_SET_DEMO, on ? 1 : 0); }
void h_open_center_page(uint8_t which) { request(ELC_CAN_ACT_OPEN_CENTER_PAGE, which); }

UiMenuHost s_host;

// ---- touch
bool s_was_down = false;
bool s_down_on_menu = false;  // the touch began with the menu open: it is the menu's
uint32_t s_down_ms = 0;

bool is_media_screen() { return s_st.media_screen == C.self_screen; }

bool hold_available() {
  return is_media_screen() || (C.on_hold && (!C.hold_enabled || C.hold_enabled()));
}

}  // namespace

void cluster_peer_begin(const ClusterPeerConfig &cfg) {
  C = cfg;
  s_host.self_screen = C.self_screen;
  s_host.theme = h_theme;
  s_host.set_theme = h_set_theme;
  s_host.center_view = h_center_view;
  s_host.set_center_view = h_set_center_view;
  s_host.interface_screen = h_interface;
  s_host.set_interface_screen = h_set_interface;
  s_host.media_screen = h_media;
  s_host.set_media_screen = h_set_media;
  s_host.media_shown = h_media_shown;
  s_host.show_media = h_show_media;
  s_host.brightness = h_brightness;
  s_host.dim_with_lights = h_dim;
  s_host.set_brightness = h_set_brightness;
  s_host.local_wifi = false;
  s_host.wifi_status = h_wifi_status;
  s_host.wifi_has_saved = h_wifi_has_saved;
  s_host.wifi_join_saved = h_wifi_join_saved;
  s_host.service_row = h_service_row;
  s_host.demo = h_demo;
  s_host.set_demo = h_set_demo;
  s_host.open_center_page = h_open_center_page;
  ui_menu_begin(&s_host);
  media_can_begin(C.send);
}

void cluster_peer_frame(const ElcCanFrame &f) {
  ElcCanClusterState st;
  if (elc_can_decode_cluster_state(f, st)) {
    s_st = st;
    s_have_st = true;
    s_st_ms = millis();
    return;
  }
  ElcCanCenterStatus cs;
  if (elc_can_decode_center_status(f, cs)) {
    s_status = cs;
    s_status_ms = millis();
    return;
  }
  if (s_name.feed(f)) {
    return;
  }
  media_can_frame(f);
}

bool cluster_peer_have_state() { return s_have_st; }
const ElcCanClusterState &cluster_peer_state() { return s_st; }

bool cluster_peer_show_media() { return is_media_screen() && (s_st.flags & ELC_CAN_CLUSTER_MEDIA_SHOWN); }

void cluster_peer_poll(uint32_t now_ms, int16_t x, int16_t y, bool down) {
  // Brightness: the center's setting, unless this screen's menu is adjusting it right now.
  const bool adjusting = ui_menu_is_open() && ui_menu_page() == UiMenuPage::Brightness;
  if (!adjusting && (s_st.brightness != s_applied_level || h_dim() != s_applied_follow)) {
    s_applied_level = s_st.brightness;
    s_applied_follow = h_dim();
    elc_backlight_set_user(s_applied_level, s_applied_follow);
  }

  // The menu lives on the interface screen only.
  if (ui_menu_is_open() && s_st.interface_screen != C.self_screen) {
    ui_menu_close();
  }

  p4_screen_touch(x, y, down);
  if (down && !s_was_down) {
    s_down_ms = now_ms;
    s_down_on_menu = ui_menu_is_open();
  }
  const uint32_t held = down ? now_ms - s_down_ms : 0;
  if (!down && s_was_down && !s_down_on_menu && !ui_menu_is_open()) {
    const uint32_t dur = now_ms - s_down_ms;
    if (dur >= 3000u && hold_available()) {
      if (is_media_screen()) {
        request(ELC_CAN_SET_MEDIA_SHOWN, cluster_peer_show_media() ? 0 : 1);
      } else {
        C.on_hold();
      }
    } else if (dur < 1000u && s_st.interface_screen == C.self_screen) {
      ui_menu_open(UiMenuPage::Home);
    }
  }
  s_was_down = down;

  // Hold ring: 3 s to the screen's hold action.
  if (down && !s_down_on_menu && !ui_menu_is_open() && held >= 350 && hold_available()) {
    const char *done = is_media_screen() ? (cluster_peer_show_media() ? "Release: show the gauge" : "Release: show media")
                                         : C.hold_message;
    ui_menu_hold_feedback(true, held >= 3000 ? 1000 : (int32_t)(held * 1000 / 3000),
                          held >= 3000 ? done : (is_media_screen() ? "Keep holding: media" : "Keep holding"));
  } else {
    ui_menu_hold_feedback(false, 0, nullptr);
  }

  media_can_poll(cluster_peer_show_media());
}
