#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <cstdio>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <cluster_i2c_slave.h>
#include <display_setup.h>
#include <elc_gauges.h>
#include <elc_watchdog.h>
#include <gauge_console.h>
#include <gauge_link.h>
#include <p4_screen.h>
#include <sensor_link.h>

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
static char s_uart_banner_line[96];
static uint32_t s_uart_banner_until_ms = 0;

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

static void drain_uart_text() {
  char uart_line[96];
  while (s_link.takeTextLine(uart_line, sizeof(uart_line))) {
    snprintf(s_uart_banner_line, sizeof(s_uart_banner_line), "%s", uart_line);
    s_uart_banner_until_ms = millis() + 3500;
    Serial.println(uart_line);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  s_link.begin(Serial1, SENSOR_UART_RX, SENSOR_UART_TX);

  Serial.println("el-camino-dash SPEEDO / ESP32-P4");

  Arduino_GFX *panel = create_tsps_display();
  if (!panel) {
    Serial.println("FATAL: display init failed");
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
  cluster_i2c_slave_begin(CLUSTER_I2C_ADDR_SPEEDO, on_cluster_i2c, nullptr);
  elc_loop_watchdog_begin();
}

void loop() {
  elc_loop_watchdog_feed();
  elc_illumination_gpio_poll();
  persist_theme_if_changed();
  s_link.poll();
  drain_uart_text();
  s_console.poll(s_link);

  const uint32_t now = millis();
  GaugeData gd;
  gauge_fill_sensors(gd, s_link, WHEEL_CIRCUMFERENCE_M);
  if (s_uart_banner_line[0] != 0 && (int32_t)(s_uart_banner_until_ms - now) > 0) {
    gd.banner = s_uart_banner_line;
  }
  gauges_show((GaugeTheme)(uint8_t)s_theme, GaugeFace::Speedo);
  gauges_update(gd, now);
  p4_screen_frame();

  s_link.poll();
  drain_uart_text();

  // Pace to ~60 fps: sleep only for what is left of a 16 ms frame; always yield at least 1 ms.
  static uint32_t s_frame_start_ms = 0;
  const uint32_t spent = millis() - s_frame_start_ms;
  delay(spent < 15 ? 16 - spent : 1);
  s_frame_start_ms = millis();
}
