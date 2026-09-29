#include "center_can.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include <elc_can.h>
#include <media_can.h>

#ifndef ELC_CAN_TX_PIN
#define ELC_CAN_TX_PIN -1
#endif
#ifndef ELC_CAN_RX_PIN
#define ELC_CAN_RX_PIN -1
#endif

static ElcCan s_can;
static uint8_t s_sensor_seq = 0;

// Setting requests (0x212) from the other screens and the console, applied by center_ui from loop().
static ElcCanSettingRequest s_req[8];
static uint8_t s_req_head = 0, s_req_n = 0;

static void on_frame(const ElcCanFrame &f, void *) {
  ElcCanSettingRequest r;
  if (elc_can_decode_setting_request(f, r)) {
    if (s_req_n < 8) {
      s_req[(s_req_head + s_req_n) % 8] = r;
      s_req_n++;
    }
    return;
  }
  media_can_frame(f);  // audio bridge: now playing, album art
}

static bool send_frame(const ElcCanFrame &f) { return s_can.running() && s_can.send(f); }

void center_can_begin() {
  s_can.onFrame(on_frame, nullptr);
  media_can_begin(send_frame);
  const bool ok = s_can.begin(ELC_CAN_TX_PIN, ELC_CAN_RX_PIN, ELC_CAN_NODE_CENTER, ElcCanNodeType::Display);
  Serial.printf("CAN: %s (TX=%d RX=%d)\n", ok ? "up" : "off", ELC_CAN_TX_PIN, ELC_CAN_RX_PIN);
}

void center_can_poll() { s_can.poll(); }

void center_can_send_sensor(const SensorPacketValues &v) {
  if (!s_can.running()) {
    return;
  }
  ElcCanSensor s;
  s.pot_adc = v.pot_adc;
  s.therm_adc = v.therm_adc;
  s.pressure_adc = v.pressure_adc;
  s.voltage_adc = v.voltage_adc;
  s.ambient_adc = v.ambient_adc;
  s.speed_rpm = v.speed_rpm;
  s.engine_rpm = v.engine_rpm;
  s.flags = v.flags;
  s.faults = v.faults;
  s.odometer_tenths_mile = v.has_odometer ? v.odometer_tenths_mile : 0xFFFFFFFFu;
  s.dte_tenths_mile = v.dte_tenths_mile;
  s.mpg_instant_x10 = v.mpg_instant_x10;
  s.mpg_average_x10 = v.mpg_average_x10;
  s.has_fuel_economy = v.has_fuel_economy;
  s.seq = s_sensor_seq++;
  ElcCanFrame frames[ELC_CAN_SENSOR_FRAMES];
  elc_can_encode_sensor(s, frames);
  for (const ElcCanFrame &f : frames) {
    s_can.send(f);
  }
}

void center_can_send_cluster(const ElcCanClusterState &st) {
  if (!s_can.running()) {
    return;
  }
  ElcCanFrame f;
  elc_can_encode_cluster_state(st, f);
  s_can.send(f);
}

void center_can_send_status(ElcCanCenterStatus st, const char *wifi_name) {
  if (!s_can.running()) {
    return;
  }
  // The name goes out when it changes (with a new serial) and every 5 s, so a screen that booted late gets it.
  static char s_name[ELC_CAN_TEXT_MAX + 1] = {0};
  static uint8_t s_serial = 0;
  static uint32_t s_name_ms = 0;
  const char *name = wifi_name ? wifi_name : "";
  const bool changed = strncmp(s_name, name, ELC_CAN_TEXT_MAX) != 0;
  if (changed) {
    snprintf(s_name, sizeof(s_name), "%s", name);
    s_serial = (uint8_t)((s_serial + 1) & 0x0F);
  }
  st.text_serial = s_serial;
  ElcCanFrame f;
  elc_can_encode_center_status(st, f);
  s_can.send(f);
  if (changed || millis() - s_name_ms >= 5000u) {
    s_name_ms = millis();
    ElcCanFrame t[ELC_CAN_TEXT_CHUNKS];
    elc_can_encode_text(ELC_CAN_ID_CENTER_TEXT, s_serial, s_name, t);
    for (const ElcCanFrame &c : t) {
      s_can.send(c);
    }
  }
}

bool center_can_take_setting(ElcCanSettingRequest *out) {
  if (!s_req_n) {
    return false;
  }
  *out = s_req[s_req_head];
  s_req_head = (uint8_t)((s_req_head + 1) % 8);
  s_req_n--;
  return true;
}

bool center_can_node_alive(uint8_t node) { return s_can.running() && s_can.nodeAlive(node); }

void center_can_describe(char *out, size_t n) { s_can.describe(out, n); }
