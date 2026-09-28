#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <esp_log.h>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <cluster_i2c_slave.h>
#include <display_setup.h>
#include <elc_gauges.h>
#include <elc_watchdog.h>
#include <gauge_console.h>
#include <gauge_link.h>
#include <p4_screen.h>
#include <p4_touch.h>
#include <sensor_link.h>

#include "theme_input.h"

SET_LOOP_TASK_STACK_SIZE(24 * 1024);

#ifndef SENSOR_UART_RX
#define SENSOR_UART_RX 5
#endif

#ifndef SENSOR_UART_TX
#define SENSOR_UART_TX 4
#endif

#ifndef WHEEL_CIRCUMFERENCE_M
#define WHEEL_CIRCUMFERENCE_M 2.1f
#endif

static SensorLink s_link;
static GaugeConsole s_console;

static const char *kTspsTag = "tsps";

/** Classic layout: true = four gauges (classic 2), false = fuel with warning lamps (classic 1). */
static bool s_classic2 = true;

// Theme arrives from the center over I2C. The slave callback runs in the I2C task, so it only
// records the value; NVS is written from loop().
static volatile ClusterTheme s_theme = ClusterTheme::Classic;
static volatile bool s_theme_dirty = false;

static void on_cluster_i2c(uint8_t reg, uint8_t val, void *) {
  if (reg == CLUSTER_REG_THEME) {
    s_theme = cluster_theme_from_reg(val);
    s_theme_dirty = true;
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
  Serial.printf("TSPS: switched to classic_%d (%s)\n", v ? 2 : 1, why);
}

void setup() {
  esp_log_level_set("*", ESP_LOG_INFO);
  ESP_LOGI(kTspsTag, "setup entry");
  Serial.begin(115200);
  delay(200);
  s_link.begin(Serial1, SENSOR_UART_RX, SENSOR_UART_TX);

  Serial.println("el-camino-dash TSPS / ESP32-P4");

  Arduino_GFX *panel = create_tsps_display();
  if (!panel) {
    Serial.println("FATAL: display init failed (wrong PlatformIO env / board for this MIPI panel?)");
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

  // theme_input_begin() opens the "tsps" NVS namespace; load the saved variant only after that.
  theme_input_begin();
  s_classic2 = theme_load_classic2();
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
  s_console.on_variant = [](uint8_t v) { set_classic2(v == 0, "console"); };
  cluster_i2c_slave_begin(CLUSTER_I2C_ADDR_TSPS, on_cluster_i2c, nullptr);

  Serial.printf("TSPS: theme=%d classic_%d (GPIO / 3s touch)\n", (int)s_theme, s_classic2 ? 2 : 1);
  elc_loop_watchdog_begin();
}

void loop() {
  elc_loop_watchdog_feed();
  elc_illumination_gpio_poll();
  persist_theme_if_changed();
  s_link.poll();
  s_console.poll(s_link);
  const uint32_t now = millis();

  bool gpio_toggled = false;
  theme_poll_gpio_toggle(&gpio_toggled);
  if (gpio_toggled && s_theme == ClusterTheme::Classic) {
    set_classic2(!s_classic2, "GPIO");
  }

  {
    int16_t tx = 0, ty = 0;
    bool finger_down = false;
    p4_touch_sample(&tx, &ty, &finger_down);
    static bool s_was_down = false;
    static uint32_t s_down_ms = 0;
    if (finger_down && !s_was_down) {
      s_down_ms = now;
    }
    if (!finger_down && s_was_down && s_theme == ClusterTheme::Classic && now - s_down_ms >= 3000u) {
      set_classic2(!s_classic2, "3s hold");
    }
    s_was_down = finger_down;
  }

  GaugeData gd;
  gauge_fill_sensors(gd, s_link, WHEEL_CIRCUMFERENCE_M);
  gauges_show((GaugeTheme)(uint8_t)s_theme, GaugeFace::Right, s_classic2 ? 0 : 1);
  gauges_update(gd, now);
  p4_screen_frame();

  s_link.poll();
  char uart_line[96];
  while (s_link.takeTextLine(uart_line, sizeof(uart_line))) {
    Serial.println(uart_line);
  }

  // Pace to ~60 fps: sleep only for what is left of a 16 ms frame; always yield at least 1 ms.
  static uint32_t s_frame_start_ms = 0;
  const uint32_t spent = millis() - s_frame_start_ms;
  delay(spent < 15 ? 16 - spent : 1);
  s_frame_start_ms = millis();
}
