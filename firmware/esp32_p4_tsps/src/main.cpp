#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <esp_log.h>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <cluster_i2c_slave.h>
#include <display_setup.h>
#include <p4_touch.h>
#include <elc_watchdog.h>
#include <sensor_link.h>

#include "theme_input.h"
#include "tsps_cluster.h"
#include "tsps_fallout.h"

SET_LOOP_TASK_STACK_SIZE(24 * 1024);

#ifndef SENSOR_UART_RX
#define SENSOR_UART_RX 5
#endif

#ifndef SENSOR_UART_TX
#define SENSOR_UART_TX 4
#endif

static SensorLink s_link;
static Arduino_GFX *gfx = nullptr;

static const char *kTspsTag = "tsps";

static bool s_classic2 = true;

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

void setup() {
  esp_log_level_set("*", ESP_LOG_INFO);
  ESP_LOGI(kTspsTag, "setup entry");
  Serial.begin(115200);
  delay(200);
  s_link.begin(Serial1, SENSOR_UART_RX, SENSOR_UART_TX);

  Serial.println("el-camino-dash TSPS / ESP32-P4");
  ESP_LOGI(kTspsTag, "calling create_tsps_display()");

  gfx = create_tsps_display();
  if (!gfx) {
    Serial.println("FATAL: display init failed (wrong PlatformIO env / board for this MIPI panel?)");
    while (true) {
      delay(1000);
    }
  }
  elc_boot_splash_framebuffer_write(gfx);
  elc_illumination_gpio_poll();

  // theme_input_begin() opens the "tsps" NVS namespace; load the saved variant only after that.
  theme_input_begin();
  s_classic2 = theme_load_classic2();
  {
    Preferences prefs;
    prefs.begin(CLUSTER_NVS_NAMESPACE, true);
    s_theme = (prefs.getUChar(CLUSTER_NVS_KEY_THEME, 0) != 0) ? ClusterTheme::Fallout : ClusterTheme::Classic;
    prefs.end();
  }
  cluster_i2c_slave_begin(CLUSTER_I2C_ADDR_TSPS, on_cluster_i2c, nullptr);

  Serial.printf("TSPS: theme=%s classic_%d (GPIO / 3s touch)\n", s_theme == ClusterTheme::Fallout ? "fallout" : "classic",
                s_classic2 ? 2 : 1);
  elc_loop_watchdog_begin();
}

void loop() {
  elc_illumination_gpio_poll();
  persist_theme_if_changed();
  s_link.poll();
  const uint32_t now = millis();

  bool gpio_toggled = false;
  theme_poll_gpio_toggle(&gpio_toggled);
  if (gpio_toggled && s_theme == ClusterTheme::Classic) {
    s_classic2 = !s_classic2;
    theme_save_classic2(s_classic2);
    Serial.printf("TSPS: switched to classic_%d (GPIO)\n", s_classic2 ? 2 : 1);
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
    if (!finger_down && s_was_down && s_theme == ClusterTheme::Classic) {
      const uint32_t dur = now - s_down_ms;
      if (dur >= 3000u) {
        s_classic2 = !s_classic2;
        theme_save_classic2(s_classic2);
        Serial.printf("TSPS: switched to classic_%d (3s hold)\n", s_classic2 ? 2 : 1);
      }
    }
    s_was_down = finger_down;
  }

  const SensorDerived d = s_link.derived();
  if (s_theme == ClusterTheme::Fallout) {
    tsps_draw_fallout(gfx, gfx->width(), gfx->height(), d);
    if (!d.valid) {
      elc_gfx_draw_no_data(gfx, gfx->width(), gfx->height());
    }
  } else {
    tsps_render_cluster(gfx, gfx->width(), gfx->height(), d, s_classic2);
  }

  s_link.poll();
  char uart_line[96];
  while (s_link.takeTextLine(uart_line, sizeof(uart_line))) {
    Serial.println(uart_line);
  }
  delay(15);
}
