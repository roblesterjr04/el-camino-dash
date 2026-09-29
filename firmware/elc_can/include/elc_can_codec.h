#pragma once

// Pack/unpack for the frames in elc_can_ids.h. No Arduino or ESP-IDF dependencies, so it is
// host-tested (test/host/run_tests.sh).

#include <stddef.h>
#include <stdint.h>

#include "elc_can_ids.h"

struct ElcCanFrame {
  uint16_t id = 0;
  uint8_t len = 0;
  uint8_t data[8] = {0};
};

/** One sensor pack reading, as carried on 0x100..0x104 (same fields as the UART frame). */
struct ElcCanSensor {
  uint16_t pot_adc = 0;
  uint16_t therm_adc = 0;
  uint16_t pressure_adc = 0;
  uint16_t voltage_adc = 0;
  uint16_t ambient_adc = 0;
  uint32_t speed_rpm = 0;
  uint32_t engine_rpm = 0;
  uint8_t flags = 0;
  uint16_t faults = 0;
  uint32_t odometer_tenths_mile = 0xFFFFFFFFu;  // 0xFFFFFFFF = unknown
  uint32_t dte_tenths_mile = 0;
  uint16_t mpg_instant_x10 = 0;
  uint16_t mpg_average_x10 = 0;
  bool has_fuel_economy = false;
  uint8_t seq = 0;  // bumps once per sensor pack frame
};

static const size_t ELC_CAN_SENSOR_FRAMES = 5;

/** Fills out[0..4] (0x100..0x104). */
void elc_can_encode_sensor(const ElcCanSensor &s, ElcCanFrame out[ELC_CAN_SENSOR_FRAMES]);

/** Collects the five sensor frames. Fields update as frames arrive; complete() once each was seen. */
class ElcCanSensorDecoder {
 public:
  /** True if the frame was a sensor frame. */
  bool feed(const ElcCanFrame &f);
  bool complete() const { return _seen == 0x1F; }
  const ElcCanSensor &value() const { return _v; }
  void reset() { _seen = 0; }

 private:
  ElcCanSensor _v;
  uint8_t _seen = 0;
};

struct ElcCanClusterState {
  uint8_t theme = 0;
  uint8_t center_subview = 0;
};
void elc_can_encode_cluster_state(const ElcCanClusterState &s, ElcCanFrame &out);
bool elc_can_decode_cluster_state(const ElcCanFrame &f, ElcCanClusterState &out);

struct ElcCanHeartbeat {
  uint8_t node = 0;
  ElcCanNodeType type = ElcCanNodeType::Unknown;
  uint16_t fw_version = 0;
  uint32_t uptime_s = 0;
  uint8_t flags = 0;
};
void elc_can_encode_heartbeat(const ElcCanHeartbeat &h, ElcCanFrame &out);
bool elc_can_decode_heartbeat(const ElcCanFrame &f, ElcCanHeartbeat &out);

// ---- audio bridge (slot range) ----
static const size_t ELC_CAN_AUDIO_TEXT_MAX = 32;   // same limit as the I2C snapshot
static const size_t ELC_CAN_AUDIO_TEXT_CHUNKS = 5; // 5 x 7 bytes >= 32 + NUL

struct ElcCanAudioTransport {
  uint16_t elapsed_s = 0;
  uint16_t duration_s = 0;
  uint8_t volume = 0;       // 0..15
  uint8_t text_serial = 0;  // 0..15, bumps when the text changes
  bool connected = false;   // phone connected
};
void elc_can_encode_audio_transport(uint8_t slot, const ElcCanAudioTransport &t, ElcCanFrame &out);
bool elc_can_decode_audio_transport(const ElcCanFrame &f, uint8_t *slot, ElcCanAudioTransport &out);

/** Splits text (NUL-terminated, cut at 32 bytes) into ELC_CAN_AUDIO_TEXT_CHUNKS frames. */
void elc_can_encode_audio_text(uint8_t slot, uint8_t serial, const char *text,
                               ElcCanFrame out[ELC_CAN_AUDIO_TEXT_CHUNKS]);

/** Reassembles the now-playing text; a new serial restarts collection. */
class ElcCanAudioTextDecoder {
 public:
  /** True when this frame completed a text (read it with text()). */
  bool feed(const ElcCanFrame &f);
  const char *text() const { return _done; }
  uint8_t serial() const { return _done_serial; }
  bool valid() const { return _have; }

 private:
  char _buf[ELC_CAN_AUDIO_TEXT_CHUNKS * 7 + 1] = {0};
  char _done[ELC_CAN_AUDIO_TEXT_MAX + 1] = {0};
  uint8_t _serial = 0xFF;
  uint8_t _done_serial = 0xFF;
  uint8_t _seen = 0;
  bool _have = false;
};
