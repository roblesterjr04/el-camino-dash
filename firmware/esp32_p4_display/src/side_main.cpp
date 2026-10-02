#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <cstdio>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <cluster_i2c_slave.h>
#include <cluster_peer.h>
#include <display_setup.h>
#include <elc_can.h>
#include <elc_gauges.h>
#include <elc_watchdog.h>
#include <gauge_console.h>
#include <gauge_link.h>
#include <media_can.h>
#include <menu_can.h>
#include <p4_screen.h>
#include <p4_touch.h>
#include <phone_can.h>
#include <sensor_link.h>
#include <ui_menu.h>

#include "display_main.h"
#include "display_role.h"
#include "side_theme_input.h"

// The left (speedometer) and right (fuel / oil / temp / charge) screens. Both read the sensor pack UART for their
// gauges and follow the center for everything else: theme and screen settings over CAN (cluster_peer.h), with the
// I2C cluster bus as the bench fallback for the theme.

#ifndef SENSOR_UART_RX
#define SENSOR_UART_RX 5
#endif

#ifndef WHEEL_CIRCUMFERENCE_M
#define WHEEL_CIRCUMFERENCE_M 2.1f
#endif

#ifndef SIDE_TOUCH_ENABLE
#define SIDE_TOUCH_ENABLE 1
#endif
#ifndef SIDE_TOUCH_SDA
#define SIDE_TOUCH_SDA 7
#endif
#ifndef SIDE_TOUCH_SCL
#define SIDE_TOUCH_SCL 8
#endif

#ifndef ELC_CAN_TX_PIN
#define ELC_CAN_TX_PIN -1
#endif
#ifndef ELC_CAN_RX_PIN
#define ELC_CAN_RX_PIN -1
#endif

static uint8_t s_role = ELC_CAN_SCREEN_LEFT;
static bool s_right = false;
static SensorLink s_link;
static GaugeConsole s_console;
static ElcCan s_can;

// Speedometer: sensor-pack text lines (e.g. FRAM messages) show briefly in the odometer slot.
static char s_banner[96];
static uint32_t s_banner_until_ms = 0;

/** Right screen, Classic theme: true = four gauges (classic 2), false = fuel with warning lamps (classic 1). */
static bool s_classic2 = true;

// Theme from the center (CAN 0x210, or I2C on the bench). The I2C callback runs in the I2C task, so it only
// records the value; NVS is written from the loop.
static volatile ClusterTheme s_theme = ClusterTheme::Classic;
static volatile bool s_theme_dirty = false;

static void on_cluster_i2c(uint8_t reg, uint8_t val, void *) {
  if (reg == CLUSTER_REG_THEME) {
    s_theme = cluster_theme_from_reg(val);
    s_theme_dirty = true;
  }
}

static bool can_send(const ElcCanFrame &f) { return s_can.running() && s_can.send(f); }
static bool can_node_alive(uint8_t node) { return s_can.running() && s_can.nodeAlive(node); }

static void set_classic2(bool v, const char *why);

static void on_can_frame(const ElcCanFrame &f, void *) {
  if (menu_can_frame(f)) {  // the console driving the menu (applied if this is the interface screen)
    return;
  }
  if (phone_can_frame(f)) {  // audio bridge: calls, pairing, the paired list
    return;
  }
  static ElcCanSensorDecoder s_sensors;  // the center's re-sent sensor pack (or a USB relay), while our UART is silent
  if (s_sensors.feed(f)) {
    if (f.id == ELC_CAN_ID_SENSOR_ECON && s_sensors.complete()) s_link.feedBus(s_sensors.value());
    return;
  }
  cluster_peer_frame(f);  // settings, center status, audio bridge
  ElcCanClusterState st;
  if (elc_can_decode_cluster_state(f, st)) {
    const ClusterTheme t = cluster_theme_from_reg(st.theme);
    if (t != s_theme) {
      s_theme = t;
      s_theme_dirty = true;
    }
    // The center owns the right screen's Classic layout (setting 11, flag bit2 = fuel + lamps).
    const bool c2 = (st.flags & ELC_CAN_CLUSTER_RIGHT_LAYOUT2) == 0;
    if (s_right && c2 != s_classic2) {
      set_classic2(c2, "center");
    }
  }
}

static void persist_theme_if_changed() {
  if (!s_theme_dirty) {
    return;
  }
  s_theme_dirty = false;
  Preferences prefs;
  prefs.begin(CLUSTER_NVS_NAMESPACE, false);
  const uint8_t v = (uint8_t)s_theme;
  if (prefs.getUChar(CLUSTER_NVS_KEY_THEME, 0xFF) != v) {
    prefs.putUChar(CLUSTER_NVS_KEY_THEME, v);
  }
  prefs.end();
}

static void set_classic2(bool v, const char *why) {
  s_classic2 = v;
  theme_save_classic2(v);
  Serial.printf("right: switched to classic_%d (%s)\n", v ? 2 : 1, why);
}

/** Switch the Classic layout: the center owns it (all screens and the console see it), so ask there and follow its
 *  answer; with no center heard, switch here only. */
static void request_classic2(bool v, const char *why) {
  if (cluster_peer_center_heard()) {
    cluster_peer_request(ELC_CAN_SET_RIGHT_LAYOUT, v ? 0 : 1);
    Serial.printf("right: asked the center for classic_%d (%s)\n", v ? 2 : 1, why);
  } else {
    set_classic2(v, why);
  }
}

static bool menu_is_interface() { return cluster_peer_state().interface_screen == s_role; }

static void drain_uart_text() {
  char line[96];
  while (s_link.takeTextLine(line, sizeof(line))) {
    if (!s_right) {
      snprintf(s_banner, sizeof(s_banner), "%s", line);
      s_banner_until_ms = millis() + 3500;
    }
    Serial.println(line);
  }
}

void side_setup(uint8_t role) {
  s_role = role;
  s_right = role == ELC_CAN_SCREEN_RIGHT;
  // Receive only: the center alone answers the sensor pack, and this board's GPIO4 is the position strap
  // (tied to GND or 3V3 through 1 kΩ on the DigiDash board), so it must never drive it.
  s_link.begin(Serial1, SENSOR_UART_RX, -1);
  s_link.setKeepalive(false);

  Serial.printf("el-camino-dash display: %s\n", s_right ? "RIGHT (fuel / oil / temp / charge)" : "LEFT (speedometer)");

  Arduino_GFX *panel = create_tsps_display();
  if (!panel) {
    Serial.println("FATAL: display init failed (wrong PlatformIO env for this panel?)");
    while (true) {
      delay(1000);
    }
  }
  elc_boot_splash_framebuffer_write(panel);
  p4_screen_begin(panel, /*legacy_layer=*/false);
  {
    const int w = panel->width(), h = panel->height(), size = w < h ? w : h;
    gauges_begin(p4_screen_gauge_parent(), (w - size) / 2, (h - size) / 2, size);
  }
  elc_illumination_gpio_poll();
#if SIDE_TOUCH_ENABLE
  p4_touch_begin(SIDE_TOUCH_SDA, SIDE_TOUCH_SCL);
#endif
  if (s_right) {
    theme_input_begin();  // opens the "tsps" NVS namespace: load the saved layout only after this
    s_classic2 = theme_load_classic2();
  }
  {
    Preferences prefs;
    prefs.begin(CLUSTER_NVS_NAMESPACE, true);
    s_theme = cluster_theme_from_reg(prefs.getUChar(CLUSTER_NVS_KEY_THEME, 0));
    prefs.end();
  }

  s_console.on_theme = [](uint8_t t) {
    s_theme = cluster_theme_from_reg(t);
    s_theme_dirty = true;
  };
  if (s_right) {
    s_console.on_variant = [](uint8_t v) { request_classic2(v == 0, "console"); };
  }
  s_console.on_can = []() {
    char st[320];
    s_can.describe(st, sizeof(st));
    Serial.println(st);
  };
  s_console.on_line = [](const char *line) {
    char reply[1024];
    if (phone_can_command(line, reply, sizeof(reply)) ||  // phone, phone demo …
        menu_can_command(line, reply, sizeof(reply))) {   // menu, menu rotate|press|back|open …
      Serial.println(reply);
      return true;
    }
    return display_role_command(line);
  };
  cluster_i2c_slave_begin(s_right ? CLUSTER_I2C_ADDR_TSPS : CLUSTER_I2C_ADDR_SPEEDO, on_cluster_i2c, nullptr);

  {
    ClusterPeerConfig pc;
    pc.self_screen = role;
    pc.node = display_role_can_node(role);
    pc.send = can_send;
    pc.node_alive = can_node_alive;
    pc.link = &s_link;
    if (s_right) {
      // Not the media screen: the 3 s hold switches the Classic layout.
      pc.on_hold = []() { request_classic2(!s_classic2, "3s hold"); };
      pc.hold_enabled = []() { return s_theme == ClusterTheme::Classic; };
      pc.hold_message = "Release: switch layout";
    }
    cluster_peer_begin(pc);
    phone_can_begin(can_send, pc.node);  // the menu's Phone page sends phone commands
    MenuCanConfig mc;
    mc.self_screen = role;
    mc.send = can_send;
    mc.is_interface = menu_is_interface;
    menu_can_begin(mc);
  }
  s_can.onFrame(on_can_frame, nullptr);
  Serial.printf("CAN: %s (TX=%d RX=%d, node 0x%02X)\n",
                s_can.begin(ELC_CAN_TX_PIN, ELC_CAN_RX_PIN, display_role_can_node(role), ElcCanNodeType::Display) ? "up" : "off",
                ELC_CAN_TX_PIN, ELC_CAN_RX_PIN, (unsigned)display_role_can_node(role));
  if (s_right) {
    Serial.printf("right: theme=%d classic_%d (3 s hold when not the media screen)\n", (int)s_theme, s_classic2 ? 2 : 1);
  }
  elc_loop_watchdog_begin();
}

void side_loop() {
  elc_loop_watchdog_feed();
  elc_illumination_gpio_poll();
  persist_theme_if_changed();
  s_can.poll();
  s_link.poll();
  drain_uart_text();
  s_console.poll(s_link);
  const uint32_t now = millis();

  if (s_right) {
    bool gpio_toggled = false;
    theme_poll_gpio_toggle(&gpio_toggled);
    if (gpio_toggled && s_theme == ClusterTheme::Classic) {
      request_classic2(!s_classic2, "GPIO");
    }
  }

  {
    int16_t tx = 0, ty = 0;
    bool down = false;
#if SIDE_TOUCH_ENABLE
    p4_touch_sample(&tx, &ty, &down);
#endif
    // A touch that starts on the call card (or while it shows) is the card's: hidden from the menu tap and the holds
    // until the finger lifts, even when the card closes first (End).
    static bool s_on_card = false;
    if (down && phone_can_card_showing()) s_on_card = true;
    if (!down) s_on_card = false;
    cluster_peer_poll(now, tx, ty, down && !s_on_card);  // menu, media / layout hold, brightness
  }
  menu_can_poll(now);  // the console's menu input and state (when this is the interface screen)

  static uint32_t s_dup_check_ms = 0;
  if (now - s_dup_check_ms >= 10000u) {
    s_dup_check_ms = now;
    if (can_node_alive(display_role_can_node(s_role))) {  // our own frames never come back: another board
      Serial.printf("display: WARNING another board on CAN is also the %s screen (check the position straps)\n",
                    display_role_name(s_role));
    }
  }

  const bool media = cluster_peer_show_media();
  if (media) {
    gauges_show((GaugeTheme)(uint8_t)s_theme, GaugeFace::Media);
  } else if (s_right) {
    gauges_show((GaugeTheme)(uint8_t)s_theme, GaugeFace::Right, s_classic2 ? 0 : 1);
  } else {
    gauges_show((GaugeTheme)(uint8_t)s_theme, GaugeFace::Speedo);
  }
  if (!ui_menu_is_open()) {  // while the menu covers the face, leave it be
    GaugeData gd;
    gauge_fill_sensors(gd, s_link, WHEEL_CIRCUMFERENCE_M);
    if (!s_right && s_banner[0] != 0 && (int32_t)(s_banner_until_ms - now) > 0) {
      gd.banner = s_banner;
    }
    if (media) {
      media_can_fill(gd, now);
    }
    gauges_update(gd, now);
  }
  phone_can_poll(cluster_peer_state().media_screen == s_role, now);  // call card on the media screen
  p4_screen_frame();

  s_link.poll();
  drain_uart_text();

  // Pace to ~60 fps: sleep only for what is left of a 16 ms frame; always yield at least 1 ms.
  static uint32_t s_frame_start_ms = 0;
  const uint32_t spent = millis() - s_frame_start_ms;
  delay(spent < 15 ? 16 - spent : 1);
  s_frame_start_ms = millis();
}
