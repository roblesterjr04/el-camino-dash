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
  uint8_t art_id = 0;       // album art ready to fetch (0 = none yet); byte 7, absent from older bridges
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

// ---- album art (JPEG from the phone), fetched over CAN on request ----
//
// A display asks with ART_REQUEST (slot, art id, first chunk). The bridge answers with ART_INFO (size) and
// then ART_DATA chunks from that point to the end, as fast as the bus allows at its low priority. Frames are
// single-shot, so the receiver keeps a bitmap and asks again from the first missing chunk.

static const size_t ELC_CAN_ART_CHUNK = 5;             // JPEG bytes per ART_DATA frame
static const size_t ELC_CAN_ART_MAX_BYTES = 48 * 1024;  // largest image a receiver accepts

struct ElcCanArtRequest {
  uint8_t slot = 0;
  uint8_t art_id = 0;
  uint16_t first_chunk = 0;
};
void elc_can_encode_art_request(const ElcCanArtRequest &r, ElcCanFrame &out);
bool elc_can_decode_art_request(const ElcCanFrame &f, ElcCanArtRequest &out);

void elc_can_encode_art_info(uint8_t slot, uint8_t art_id, uint32_t size, ElcCanFrame &out);
bool elc_can_decode_art_info(const ElcCanFrame &f, uint8_t *slot, uint8_t *art_id, uint32_t *size);

/** One ART_DATA frame for chunk idx of an image of `size` bytes (the last chunk may be short). */
void elc_can_encode_art_data(uint8_t slot, uint8_t art_id, uint16_t idx, const uint8_t *image, uint32_t size,
                             ElcCanFrame &out);
static inline uint16_t elc_can_art_chunks(uint32_t size) {
  return (uint16_t)((size + ELC_CAN_ART_CHUNK - 1) / ELC_CAN_ART_CHUNK);
}

/** Reassembles one image into a caller-owned buffer. */
class ElcCanArtReceiver {
 public:
  /** Start collecting art_id from slot into buf (cap bytes). Waits for ART_INFO before taking data. */
  void start(uint8_t slot, uint8_t art_id, uint8_t *buf, size_t cap);
  void reset() { start(0, 0, nullptr, 0); }
  /** True if the frame belonged to the image being collected. */
  bool feed(const ElcCanFrame &f);

  uint8_t art_id() const { return _id; }
  bool sized() const { return _size != 0; }
  uint32_t size() const { return _size; }
  /** The image does not fit the buffer (or is empty): give up on this art id. */
  bool too_big() const { return _too_big; }
  bool complete() const { return _size != 0 && _have == elc_can_art_chunks(_size); }
  uint16_t chunks_have() const { return _have; }
  /** First chunk not received yet (the chunk count when complete). */
  uint16_t first_missing() const;

 private:
  static const size_t kMaxChunks = (ELC_CAN_ART_MAX_BYTES + ELC_CAN_ART_CHUNK - 1) / ELC_CAN_ART_CHUNK;
  uint8_t *_buf = nullptr;
  size_t _cap = 0;
  uint8_t _slot = 0, _id = 0;
  uint32_t _size = 0;
  uint16_t _have = 0;
  bool _too_big = false;
  uint8_t _bits[(kMaxChunks + 7) / 8] = {0};
};
