#include "center_can.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include <elc_can.h>

#ifndef ELC_CAN_TX_PIN
#define ELC_CAN_TX_PIN -1
#endif
#ifndef ELC_CAN_RX_PIN
#define ELC_CAN_RX_PIN -1
#endif

static ElcCan s_can;
static uint8_t s_sensor_seq = 0;

// Audio bridge state from its slot frames (only one bridge is expected; the newest one wins).
static ElcCanAudioTextDecoder s_audio_text;
static ElcCanAudioTransport s_audio_tr;
static uint32_t s_audio_ms = 0;
static bool s_audio_have = false;

static void on_frame(const ElcCanFrame &f, void *) {
  uint8_t slot = 0;
  ElcCanAudioTransport t;
  if (elc_can_decode_audio_transport(f, &slot, t)) {
    s_audio_tr = t;
    s_audio_ms = millis();
    s_audio_have = true;
    return;
  }
  (void)s_audio_text.feed(f);
}

void center_can_begin() {
  s_can.onFrame(on_frame, nullptr);
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

void center_can_send_cluster(uint8_t theme, uint8_t center_subview) {
  if (!s_can.running()) {
    return;
  }
  ElcCanFrame f;
  elc_can_encode_cluster_state(ElcCanClusterState{theme, center_subview}, f);
  s_can.send(f);
}

bool center_can_audio(char *line, size_t n, uint16_t *elapsed_s, uint16_t *duration_s, uint8_t *vol_0_15,
                      uint32_t max_age_ms) {
  if (!s_audio_have || millis() - s_audio_ms > max_age_ms) {
    return false;
  }
  if (line && n) {
    // Only a text that matches the transport's serial is current (a new track may still be arriving).
    const bool current = s_audio_text.valid() && s_audio_text.serial() == s_audio_tr.text_serial;
    snprintf(line, n, "%s", current ? s_audio_text.text() : "");
  }
  if (elapsed_s) *elapsed_s = s_audio_tr.elapsed_s;
  if (duration_s) *duration_s = s_audio_tr.duration_s;
  if (vol_0_15) *vol_0_15 = s_audio_tr.volume > 15 ? 15 : s_audio_tr.volume;
  return true;
}

void center_can_describe(char *out, size_t n) { s_can.describe(out, n); }
