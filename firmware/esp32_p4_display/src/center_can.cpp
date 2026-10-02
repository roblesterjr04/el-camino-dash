#include "center_can.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include <elc_can.h>
#include <chan_screen.h>
#include <media_can.h>
#include <menu_can.h>
#include <phone_can.h>

#include "center_gps.h"
#include "center_time.h"

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
  if (menu_can_frame(f)) {  // the console driving the menu (applied if the center is the interface screen)
    return;
  }
  // Sensor and time frames are the center's own; heard from someone else they come from a USB relay (the bench),
  // and stand in while the sensor UART or GPS is silent.
  static ElcCanSensorDecoder s_sensors;
  if (s_sensors.feed(f)) {
    if (f.id == ELC_CAN_ID_SENSOR_ECON && s_sensors.complete()) center_sensor_from_bus(s_sensors.value());
    return;
  }
  ElcCanTime t;
  if (elc_can_decode_time(f, t)) {
    if (t.flags & ELC_CAN_TIME_VALID) center_time_from_bus(t.utc, t.offset_min);
    return;
  }
  if (phone_can_frame(f)) {  // audio bridge: calls, pairing, the paired list
    return;
  }
  media_can_frame(f);  // audio bridge: now playing, album art
}

static bool send_frame(const ElcCanFrame &f) { return s_can.running() && s_can.send(f); }

bool center_can_send(const ElcCanFrame &f) { return send_frame(f); }

void center_can_begin() {
  s_can.onFrame(on_frame, nullptr);
  media_can_begin(send_frame);
  phone_can_begin(send_frame, ELC_CAN_NODE_CENTER);  // the menu's Phone page sends phone commands
  const bool ok = s_can.begin(ELC_CAN_TX_PIN, ELC_CAN_RX_PIN, ELC_CAN_NODE_CENTER, ElcCanNodeType::Display);
  Serial.printf("CAN: %s (TX=%d RX=%d)\n", ok ? "up" : "off", ELC_CAN_TX_PIN, ELC_CAN_RX_PIN);
  chan_screen_begin(s_can, ELC_CAN_SCREEN_CENTER);  // the channel protocol: announce, accessory pages, LOG
}

void center_can_poll() {
  s_can.poll();
  chan_screen_poll(millis());
}

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

void center_can_send_status(const ElcCanCenterStatus &st) {
  if (!s_can.running()) {
    return;
  }
  ElcCanFrame f;
  elc_can_encode_center_status(st, f);
  s_can.send(f);
}

void center_can_send_time(const ElcCanTime &t) {
  if (!s_can.running()) {
    return;
  }
  ElcCanFrame f;
  elc_can_encode_time(t, f);
  s_can.send(f);
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
