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

/** Cluster-wide settings and state, owned by the center (0x210). A 2-byte frame from older firmware decodes
 *  with the defaults below (menu and media on the center, full brightness). */
static const uint8_t ELC_CAN_CLUSTER_DIM_WITH_LIGHTS = 0x01;  // flags: backlight follows the headlight input
static const uint8_t ELC_CAN_CLUSTER_MEDIA_SHOWN = 0x02;      // flags: the media screen shows its media view
struct ElcCanClusterState {
  uint8_t theme = 0;
  uint8_t center_subview = 0;
  uint8_t interface_screen = ELC_CAN_SCREEN_CENTER;  // the screen with the on-screen menu
  uint8_t media_screen = ELC_CAN_SCREEN_CENTER;      // the screen with the media view
  uint8_t brightness = 100;                          // user level, 10..100 %
  uint8_t flags = ELC_CAN_CLUSTER_DIM_WITH_LIGHTS;
};
void elc_can_encode_cluster_state(const ElcCanClusterState &s, ElcCanFrame &out);
bool elc_can_decode_cluster_state(const ElcCanFrame &f, ElcCanClusterState &out);

/** Settings a display or the console can ask the center to change (0x212). */
enum ElcCanSetting : uint8_t {
  ELC_CAN_SET_THEME = 1,             // 0 Classic, 1 Fallout, 2 Modern
  ELC_CAN_SET_CENTER_VIEW = 2,       // CenterViewMode
  ELC_CAN_SET_INTERFACE_SCREEN = 3,  // ELC_CAN_SCREEN_*
  ELC_CAN_SET_MEDIA_SCREEN = 4,      // ELC_CAN_SCREEN_*
  ELC_CAN_SET_BRIGHTNESS = 5,        // 10..100
  ELC_CAN_SET_DIM_WITH_LIGHTS = 6,   // 0 / 1
  ELC_CAN_SET_MEDIA_SHOWN = 7,       // 0 / 1: media view on the media screen
  ELC_CAN_SET_DEMO = 8,              // 0 / 1: simulated drive cycle
  ELC_CAN_ACT_WIFI_JOIN_SAVED = 9,   // action (value ignored)
  ELC_CAN_ACT_OPEN_CENTER_PAGE = 10, // action: open a center-only page there (1 Wi-Fi, 2 sensor log, 3 GPS data, 4 phone)
};
struct ElcCanSettingRequest {
  uint8_t setting = 0;
  uint8_t value = 0;
  uint8_t from_node = 0;
};
void elc_can_encode_setting_request(const ElcCanSettingRequest &r, ElcCanFrame &out);
bool elc_can_decode_setting_request(const ElcCanFrame &f, ElcCanSettingRequest &out);

/** What the center knows that the other displays cannot see (0x213), for a menu on another screen. */
static const uint8_t ELC_CAN_STATUS_WIFI = 0x01;        // Wi-Fi connected (name in 0x214)
static const uint8_t ELC_CAN_STATUS_GPS_FIX = 0x02;
static const uint8_t ELC_CAN_STATUS_GPS_NMEA = 0x04;    // NMEA arriving (fix or not)
static const uint8_t ELC_CAN_STATUS_AUDIO = 0x08;       // audio bridge heard
static const uint8_t ELC_CAN_STATUS_DEMO = 0x10;        // simulated drive cycle on
static const uint8_t ELC_CAN_STATUS_SAVED_WIFI = 0x20;  // a saved network exists (Join saved makes sense)
static const uint8_t ELC_CAN_STATUS_JOINING = 0x40;     // joining a network now
struct ElcCanCenterStatus {
  uint8_t flags = 0;
  int8_t rssi = 0;
  uint8_t text_serial = 0;  // 0..15, bumps when the Wi-Fi name changes
};
void elc_can_encode_center_status(const ElcCanCenterStatus &s, ElcCanFrame &out);
bool elc_can_decode_center_status(const ElcCanFrame &f, ElcCanCenterStatus &out);

/** Short text in ELC_CAN_TEXT_CHUNKS frames of [serial << 4 | chunk] + 7 bytes (32 bytes max, zero padded). */
static const size_t ELC_CAN_TEXT_MAX = 32;
static const size_t ELC_CAN_TEXT_CHUNKS = 5;
void elc_can_encode_text(uint16_t id, uint8_t serial, const char *text, ElcCanFrame out[ELC_CAN_TEXT_CHUNKS]);
/** Reassembles text sent with elc_can_encode_text on one CAN ID. */
class ElcCanTextDecoder {
 public:
  explicit ElcCanTextDecoder(uint16_t id) : _id(id) {}
  /** True when this frame completed a text. */
  bool feed(const ElcCanFrame &f);
  const char *text() const { return _done; }
  uint8_t serial() const { return _done_serial; }
  bool valid() const { return _have; }

 private:
  uint16_t _id;
  char _buf[ELC_CAN_TEXT_CHUNKS * 7 + 1] = {0};
  char _done[ELC_CAN_TEXT_MAX + 1] = {0};
  uint8_t _serial = 0xFF, _done_serial = 0xFF, _seen = 0;
  bool _have = false;
};

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
// A display asks with ART_REQUEST (slot, art id, first chunk, variant). The bridge answers with ART_INFO (size) and
// then ART_DATA chunks from that point to the end, paced (see the bridge). Frames are single-shot, so the receiver
// keeps a bitmap and asks again from the first missing chunk. Each variant is its own stream (the variant rides in
// the top bits of the chunk number), so a center and a console can fetch different ones at the same time.
// ART_INFO with size 0 means the bridge has no such variant for that art id.

enum ElcCanArtVariant : uint8_t {
  ELC_CAN_ART_ORIGINAL = 0,  // the phone's JPEG as received (typically 200x200)
  ELC_CAN_ART_JPEG128 = 1,   // 128x128 JPEG, centre-cropped (fast first picture on the center)
  ELC_CAN_ART_GRAY48 = 2,    // 48x48 grey, 4 bits per pixel, row-major, left pixel in the high nibble (console OLED)
};
static const size_t ELC_CAN_ART_GRAY48_BYTES = 48 * 48 / 2;

static const size_t ELC_CAN_ART_CHUNK = 5;             // JPEG bytes per ART_DATA frame
static const size_t ELC_CAN_ART_MAX_BYTES = 48 * 1024;  // largest image a receiver accepts

struct ElcCanArtRequest {
  uint8_t slot = 0;
  uint8_t art_id = 0;
  uint16_t first_chunk = 0;
  uint8_t variant = ELC_CAN_ART_ORIGINAL;  // absent (4-byte request) = original
};
void elc_can_encode_art_request(const ElcCanArtRequest &r, ElcCanFrame &out);
bool elc_can_decode_art_request(const ElcCanFrame &f, ElcCanArtRequest &out);

void elc_can_encode_art_info(uint8_t slot, uint8_t art_id, uint8_t variant, uint32_t size, ElcCanFrame &out);
bool elc_can_decode_art_info(const ElcCanFrame &f, uint8_t *slot, uint8_t *art_id, uint8_t *variant, uint32_t *size);

/** One ART_DATA frame for chunk idx (< 16384) of an image of `size` bytes (the last chunk may be short). */
void elc_can_encode_art_data(uint8_t slot, uint8_t art_id, uint8_t variant, uint16_t idx, const uint8_t *image,
                             uint32_t size, ElcCanFrame &out);
static inline uint16_t elc_can_art_chunks(uint32_t size) {
  return (uint16_t)((size + ELC_CAN_ART_CHUNK - 1) / ELC_CAN_ART_CHUNK);
}

/** Reassembles one image into a caller-owned buffer. */
class ElcCanArtReceiver {
 public:
  /** Start collecting `variant` of art_id from slot into buf (cap bytes). Waits for ART_INFO before taking data. */
  void start(uint8_t slot, uint8_t art_id, uint8_t variant, uint8_t *buf, size_t cap);
  void reset() { start(0, 0, 0, nullptr, 0); }
  /** True if the frame belonged to the image being collected. */
  bool feed(const ElcCanFrame &f);

  uint8_t art_id() const { return _id; }
  uint8_t variant() const { return _variant; }
  bool sized() const { return _size != 0; }
  uint32_t size() const { return _size; }
  /** The bridge has no such variant (size 0), or the image does not fit the buffer: give up on it. */
  bool refused() const { return _too_big; }
  bool complete() const { return _size != 0 && _have == elc_can_art_chunks(_size); }
  uint16_t chunks_have() const { return _have; }
  /** First chunk not received yet (the chunk count when complete). */
  uint16_t first_missing() const;

 private:
  static const size_t kMaxChunks = (ELC_CAN_ART_MAX_BYTES + ELC_CAN_ART_CHUNK - 1) / ELC_CAN_ART_CHUNK;
  uint8_t *_buf = nullptr;
  size_t _cap = 0;
  uint8_t _slot = 0, _id = 0, _variant = 0;
  uint32_t _size = 0;
  uint16_t _have = 0;
  bool _too_big = false;
  uint8_t _bits[(kMaxChunks + 7) / 8] = {0};
};
