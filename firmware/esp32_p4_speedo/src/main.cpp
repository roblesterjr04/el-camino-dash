#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <cstdio>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <cluster_i2c_slave.h>
#include <display_setup.h>
#include <el_camino_speed.h>
#include <elc_watchdog.h>
#include <sensor_link.h>

#include "speedo_cluster.h"
#include "speedo_fallout.h"

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
static Arduino_GFX *gfx = nullptr;
static char s_uart_banner_line[96];
static uint32_t s_uart_banner_until_ms = 0;

// Theme arrives from the center over I2C. The slave callback runs in the I2C task, so it only
// records the value; NVS is written from loop().
static volatile ClusterTheme s_theme = ClusterTheme::Classic;
static volatile bool s_theme_dirty = false;

static void on_cluster_i2c(uint8_t reg, uint8_t val, void *) {
  if (reg == CLUSTER_REG_THEME) {
    s_theme = (val != 0) ? ClusterTheme::Fallout : ClusterTheme::Classic;
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
  const uint8_t v = (s_theme == ClusterTheme::Fallout) ? 1 : 0;
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

  gfx = create_tsps_display();
  if (!gfx) {
    Serial.println("FATAL: display init failed");
    while (true) {
      delay(1000);
    }
  }
  elc_boot_splash_framebuffer_write(gfx);
  elc_illumination_gpio_poll();
  {
    Preferences prefs;
    prefs.begin(CLUSTER_NVS_NAMESPACE, true);
    s_theme = (prefs.getUChar(CLUSTER_NVS_KEY_THEME, 0) != 0) ? ClusterTheme::Fallout : ClusterTheme::Classic;
    prefs.end();
  }
  cluster_i2c_slave_begin(CLUSTER_I2C_ADDR_SPEEDO, on_cluster_i2c, nullptr);
  elc_loop_watchdog_begin();
}

void loop() {
  elc_loop_watchdog_feed();
  elc_illumination_gpio_poll();
  persist_theme_if_changed();
  s_link.poll();
  drain_uart_text();

  const uint32_t now = millis();
  const bool fresh = s_link.fresh();
  const SensorDerived d = s_link.derived();
  const float mph = fresh ? elc_speed_rpm_to_mph(s_link.last().speed_rpm, WHEEL_CIRCUMFERENCE_M, 1.0f) : 0.0f;
  float odometer_miles = 0.0f;
  const bool odo_ok = s_link.odometerMiles(&odometer_miles);

  const char *uart_banner =
      (s_uart_banner_line[0] != 0 && (int32_t)(s_uart_banner_until_ms - now) > 0) ? s_uart_banner_line : nullptr;
  if (s_theme == ClusterTheme::Fallout) {
    speedo_draw_fallout(gfx, gfx->width(), gfx->height(), mph, d);
    if (!fresh) {
      elc_gfx_draw_no_data(gfx, gfx->width(), gfx->height());
    }
  } else {
    speedo_render(gfx, gfx->width(), gfx->height(), mph, odometer_miles, odo_ok, uart_banner, fresh);
  }

  s_link.poll();
  drain_uart_text();
  delay(15);
}
