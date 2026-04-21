#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <esp_log.h>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <cluster_i2c_slave.h>
#include <display_setup.h>
#include <p4_touch.h>
#include <sensor_packet.h>

#include "theme_input.h"
#include "tsps_cluster.h"
#include "tsps_fallout.h"

SET_LOOP_TASK_STACK_SIZE(24 * 1024);

#ifndef SENSOR_UART_NUM
#define SENSOR_UART_NUM 1
#endif

#ifndef SENSOR_UART_RX
#define SENSOR_UART_RX 5
#endif

#ifndef SENSOR_UART_TX
#define SENSOR_UART_TX 4
#endif

#if SENSOR_UART_NUM == 1
HardwareSerial &SensorSerial = Serial1;
#elif SENSOR_UART_NUM == 2
HardwareSerial &SensorSerial = Serial2;
#else
#error "Set SENSOR_UART_NUM to 1 or 2"
#endif

static SensorPacketParser parser;
static Arduino_GFX *gfx = nullptr;

static const char *kTspsTag = "tsps";

#ifndef SENSOR_PACKET_STALE_MS
#define SENSOR_PACKET_STALE_MS 400u
#endif

static Preferences s_prefs_cluster;
static bool s_classic2 = true;
static ClusterTheme s_theme = ClusterTheme::Classic;

static void on_cluster_i2c(uint8_t reg, uint8_t val, void *) {
  if (reg == CLUSTER_REG_THEME) {
    s_theme = (ClusterTheme)val;
    s_prefs_cluster.begin(CLUSTER_NVS_NAMESPACE, false);
    s_prefs_cluster.putUChar(CLUSTER_NVS_KEY_THEME, val);
  }
}

void setup() {
  esp_log_level_set("*", ESP_LOG_INFO);
  ESP_LOGI(kTspsTag, "setup entry");
  Serial.begin(115200);
  delay(200);
  SensorSerial.setRxBufferSize(2048);
  SensorSerial.begin(115200, SERIAL_8N1, SENSOR_UART_RX, SENSOR_UART_TX);
  parser.begin(SensorSerial);

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

  s_classic2 = theme_load_classic2();
  theme_input_begin();
  s_prefs_cluster.begin(CLUSTER_NVS_NAMESPACE, false);
  s_theme = (s_prefs_cluster.getUChar(CLUSTER_NVS_KEY_THEME, 0) != 0) ? ClusterTheme::Fallout : ClusterTheme::Classic;
  cluster_i2c_slave_begin(CLUSTER_I2C_ADDR_TSPS, on_cluster_i2c, nullptr);

  Serial.printf("TSPS: theme=%s classic_%d (GPIO / 3s touch)\n", s_theme == ClusterTheme::Fallout ? "fallout" : "classic",
                s_classic2 ? 2 : 1);
}

void loop() {
  SensorPacketValues raw{};
  bool got = parser.poll(raw, true);

  static uint32_t last_keepalive_ms = 0;
  uint32_t now = millis();
  if (now - last_keepalive_ms >= 350) {
    SensorSerial.write(PI_HANDSHAKE_BYTE);
    last_keepalive_ms = now;
  }

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
    (void)tx;
    (void)ty;
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

  static SensorDerived held{};
  static uint32_t last_pkt_ms = 0;
  static bool have_pkt = false;

  if (got) {
    held = SensorPacketParser::derive(raw);
    last_pkt_ms = now;
    have_pkt = true;
  }

  SensorDerived d;
  if (have_pkt && (now - last_pkt_ms) <= SENSOR_PACKET_STALE_MS) {
    d = held;
  } else {
    d.valid = false;
    d.fuel_pct = 0.0f;
    d.oil_psi = 0.0f;
    d.coolant_temp_f = 100.0f;
    d.system_amp = 0.0f;
  }

  if (s_theme == ClusterTheme::Fallout) {
    tsps_draw_fallout(gfx, gfx->width(), gfx->height(), d);
  } else {
    tsps_render_cluster(gfx, gfx->width(), gfx->height(), d, s_classic2);
  }

  SensorPacketValues post{};
  if (parser.poll(post, true)) {
    held = SensorPacketParser::derive(post);
    last_pkt_ms = millis();
    have_pkt = true;
  }

  delay(15);
}
