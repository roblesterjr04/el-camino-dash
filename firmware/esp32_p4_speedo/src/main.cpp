#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <cluster_i2c_slave.h>
#include <display_setup.h>
#include <el_camino_speed.h>
#include <sensor_packet.h>

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

static void on_cluster_i2c(uint8_t reg, uint8_t val, void *) {
  if (reg == CLUSTER_REG_THEME) {
    s_theme = (ClusterTheme)val;
    s_prefs.begin(CLUSTER_NVS_NAMESPACE, false);
    s_prefs.putUChar(CLUSTER_NVS_KEY_THEME, val);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("el-camino-dash SPEEDO / ESP32-P4");

  gfx = create_tsps_display();
  if (!gfx) {
    Serial.println("FATAL: display init failed");
    while (true) {
      delay(1000);
    }
  }
  elc_boot_splash_framebuffer_write(gfx);
  s_prefs.begin(CLUSTER_NVS_NAMESPACE, false);
  s_theme = (s_prefs.getUChar(CLUSTER_NVS_KEY_THEME, 0) != 0) ? ClusterTheme::Fallout : ClusterTheme::Classic;
  cluster_i2c_slave_begin(CLUSTER_I2C_ADDR_SPEEDO, on_cluster_i2c, nullptr);

  SensorSerial.begin(115200, SERIAL_8N1, SENSOR_UART_RX, SENSOR_UART_TX);
  parser.begin(SensorSerial);
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

  if (s_theme == ClusterTheme::Fallout) {
    speedo_draw_fallout(gfx, gfx->width(), gfx->height(), mph, d);
  } else {
    speedo_render(gfx, gfx->width(), gfx->height(), mph);
  }
  delay(15);
}
