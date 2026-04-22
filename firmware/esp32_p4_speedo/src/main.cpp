#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <cstdio>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <cluster_i2c_slave.h>
#include <display_setup.h>
#include <el_camino_speed.h>
#include <sensor_packet.h>

#include "speedo_cluster.h"
#include "speedo_fallout.h"

#ifndef CLUSTER_NVS_KEY_SPEEDO_ODO
#define CLUSTER_NVS_KEY_SPEEDO_ODO "odo"
#endif
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
#ifndef SPEED_PULSES_PER_REV
#define SPEED_PULSES_PER_REV 1.0f
#endif

#ifndef SENSOR_PACKET_STALE_MS
#define SENSOR_PACKET_STALE_MS 400u
#endif

HardwareSerial &SensorSerial = Serial1;
static SensorPacketParser parser;
static Arduino_GFX *gfx = nullptr;
static Preferences s_prefs;
static ClusterTheme s_theme = ClusterTheme::Classic;
static float s_odometer_miles = 0.0f;
static uint32_t s_last_odo_save_ms = 0;
static bool s_persisted_uart_odo_this_session = false;
static char s_uart_banner_line[96];
static uint32_t s_uart_banner_until_ms = 0;

/** Commit odometer to NVS; must call end() so values survive reboot on ESP32. */
static void persist_odometer_miles_to_nvs(float miles) {
  s_prefs.begin(CLUSTER_NVS_NAMESPACE, false);
  s_prefs.putFloat(CLUSTER_NVS_KEY_SPEEDO_ODO, miles);
  s_prefs.end();
}

static void on_cluster_i2c(uint8_t reg, uint8_t val, void *) {
  if (reg == CLUSTER_REG_THEME) {
    s_theme = (ClusterTheme)val;
    s_prefs.begin(CLUSTER_NVS_NAMESPACE, false);
    s_prefs.putUChar(CLUSTER_NVS_KEY_THEME, val);
    s_prefs.end();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  SensorSerial.setRxBufferSize(2048);
  SensorSerial.begin(115200, SERIAL_8N1, SENSOR_UART_RX, SENSOR_UART_TX);
  parser.begin(SensorSerial);

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
  s_prefs.begin(CLUSTER_NVS_NAMESPACE, false);
  s_theme = (s_prefs.getUChar(CLUSTER_NVS_KEY_THEME, 0) != 0) ? ClusterTheme::Fallout : ClusterTheme::Classic;
  s_prefs.end();
  // Display is 000000 until a v3 UART packet with odometer; do not show NVS from a prior session.
  s_odometer_miles = 0.0f;
  s_last_odo_save_ms = millis();
  cluster_i2c_slave_begin(CLUSTER_I2C_ADDR_SPEEDO, on_cluster_i2c, nullptr);
}

void loop() {
  elc_illumination_gpio_poll();
  SensorPacketValues raw{};
  bool got = parser.poll(raw, true);
  char uart_line[96];
  while (parser.takePendingTextLine(uart_line, sizeof(uart_line))) {
    snprintf(s_uart_banner_line, sizeof(s_uart_banner_line), "%s", uart_line);
    s_uart_banner_until_ms = millis() + 3500;
    Serial.println(uart_line);
  }
  static uint32_t last_keepalive_ms = 0;
  uint32_t now = millis();
  if (now - last_keepalive_ms >= 350) {
    SensorSerial.write(PI_HANDSHAKE_BYTE);
    last_keepalive_ms = now;
  }

  static SensorPacketValues held{};
  static uint32_t last_pkt = 0;
  static bool have = false;
  if (got) {
    held = raw;
    last_pkt = now;
    have = true;
  }

  float mph = 0.0f;
  SensorDerived d{};
  if (have && (now - last_pkt) <= SENSOR_PACKET_STALE_MS) {
    mph = elc_speed_rpm_to_mph(held.speed_rpm, WHEEL_CIRCUMFERENCE_M, SPEED_PULSES_PER_REV);
    d = SensorPacketParser::derive(held);
  } else {
    d.valid = false;
    d.fuel_pct = 0.0f;
    d.coolant_temp_f = 100.0f;
  }

  const bool pkt_ok = have && (now - last_pkt) <= SENSOR_PACKET_STALE_MS;
  const bool sensor_pack_odo_ok = pkt_ok && held.has_odometer;
  float display_odometer_miles = 0.0f;
  if (sensor_pack_odo_ok) {
    display_odometer_miles = (float)held.odometer_tenths_mile * 0.1f;
    s_odometer_miles = display_odometer_miles;
    if (!s_persisted_uart_odo_this_session) {
      s_persisted_uart_odo_this_session = true;
      persist_odometer_miles_to_nvs(s_odometer_miles);
      s_last_odo_save_ms = now;
    }
  }
  if (sensor_pack_odo_ok && (now - s_last_odo_save_ms >= 30000U)) {
    s_last_odo_save_ms = now;
    persist_odometer_miles_to_nvs(s_odometer_miles);
  }

  const char *uart_banner =
      (s_uart_banner_line[0] != 0 && now < s_uart_banner_until_ms) ? s_uart_banner_line : nullptr;
  if (s_theme == ClusterTheme::Fallout) {
    speedo_draw_fallout(gfx, gfx->width(), gfx->height(), mph, d);
  } else {
    speedo_render(gfx, gfx->width(), gfx->height(), mph, display_odometer_miles, sensor_pack_odo_ok, uart_banner);
  }

  SensorPacketValues post{};
  if (parser.poll(post, true)) {
    held = post;
    last_pkt = millis();
    have = true;
  }
  while (parser.takePendingTextLine(uart_line, sizeof(uart_line))) {
    snprintf(s_uart_banner_line, sizeof(s_uart_banner_line), "%s", uart_line);
    s_uart_banner_until_ms = millis() + 3500;
    Serial.println(uart_line);
  }

  delay(15);
}
