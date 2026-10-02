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
static const uint8_t ELC_CAN_CLUSTER_RIGHT_LAYOUT2 = 0x04;    // flags: the right screen's second Classic layout (fuel + lamps)
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
  // 9 reserved (was Wi-Fi join saved)
  ELC_CAN_ACT_OPEN_CENTER_PAGE = 10, // action: open a center-only page there (2 sensor log, 3 GPS data, 4 phone; 1 reserved, was Wi-Fi)
  ELC_CAN_SET_RIGHT_LAYOUT = 11,     // 0 four gauges, 1 fuel + lamps (Classic only; 0x210 flag bit2)
};
struct ElcCanSettingRequest {
  uint8_t setting = 0;
  uint8_t value = 0;
  uint8_t from_node = 0;
};
void elc_can_encode_setting_request(const ElcCanSettingRequest &r, ElcCanFrame &out);
bool elc_can_decode_setting_request(const ElcCanFrame &f, ElcCanSettingRequest &out);

/** What the center knows that the other displays cannot see (0x213), for a menu on another screen. */
// 0x01 reserved (was Wi-Fi connected)
static const uint8_t ELC_CAN_STATUS_GPS_FIX = 0x02;
static const uint8_t ELC_CAN_STATUS_GPS_NMEA = 0x04;    // NMEA arriving (fix or not)
static const uint8_t ELC_CAN_STATUS_AUDIO = 0x08;       // audio bridge heard
static const uint8_t ELC_CAN_STATUS_DEMO = 0x10;        // simulated drive cycle on
// 0x20, 0x40 reserved (were saved Wi-Fi network, joining)
static const uint8_t ELC_CAN_STATUS_REVERSE = 0x80;     // in reverse: the center shows the rear camera
struct ElcCanCenterStatus {
  uint8_t flags = 0;  // bytes 1 and 2 are reserved (were Wi-Fi RSSI and name serial), sent as 0
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

/** Which accessory ranges belong to a live audio bridge, learned from heartbeats (node type AudioBridge). Node numbers
 *  come from the slot a card sits in, so any range may be a bridge, and any other card's private messages share the
 *  same offsets: a receiver feeds every frame here and drops range frames for which ok() is false. */
class ElcCanBridgeSeen {
 public:
  static const uint32_t kMaxAgeMs = 3000;
  /** Call with every received frame; returns true when it was a heartbeat. */
  bool feed(const ElcCanFrame &f, uint32_t now_ms);
  /** True when range `slot` (node - 0x10) sent an AudioBridge heartbeat within kMaxAgeMs. */
  bool ok(int slot, uint32_t now_ms) const;
  /** True when f is in an accessory range that is not a live bridge (the receiver should ignore it). */
  bool reject(const ElcCanFrame &f, uint32_t now_ms) const {
    const int s = elc_can_slot_of(f.id);
    return s >= 0 && !ok(s, now_ms);
  }

 private:
  uint32_t _ms[ELC_CAN_RANGE_COUNT] = {0};
  bool _bridge[ELC_CAN_RANGE_COUNT] = {false};
};

// ---- audio bridge (slot range) ----
static const size_t ELC_CAN_AUDIO_TEXT_MAX = 32;   // same limit as the I2C snapshot
static const size_t ELC_CAN_AUDIO_TEXT_CHUNKS = 5; // 5 x 7 bytes >= 32 + NUL

struct ElcCanAudioTransport {
  uint16_t elapsed_s = 0;
  uint16_t duration_s = 0;
  uint8_t volume = 0;       // 0..15
  uint8_t text_serial = 0;  // 0..15, bumps when the text changes
  bool connected = false;   // phone connected
  bool playing = false;     // the phone says it plays (AVRCP play status; flags bit1, 0 from older bridges)
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

// ---- the console driving the round-screen menu ----
//
// The console sends MENU_INPUT; the screen that is the interface screen (0x210) applies it to its menu, opening it on
// the page named by an OPEN event. That screen sends MENU_STATE (on change and every second while open, once when it
// closes) with two short texts for the console's OLED: the focused item or page, and its value or state.

enum ElcCanMenuEvent : uint8_t {
  ELC_CAN_MENU_ROTATE = 1,  // value = steps (+ clockwise)
  ELC_CAN_MENU_PRESS = 2,
  ELC_CAN_MENU_BACK = 3,    // back a page; on home, close
  ELC_CAN_MENU_OPEN = 4,    // value = page (ElcCanMenuPage)
};
/** Pages of the round-screen menu (UiMenuPage numbering). */
enum ElcCanMenuPage : uint8_t {
  ELC_CAN_PAGE_HOME = 0,
  // 1 reserved (was Wi-Fi); the screens ignore it
  ELC_CAN_PAGE_BRIGHTNESS = 2,
  ELC_CAN_PAGE_THEME = 3,
  ELC_CAN_PAGE_SCREENS = 4,
  ELC_CAN_PAGE_SERVICE = 5,
  ELC_CAN_PAGE_PHONE = 6,
  ELC_CAN_PAGE_ACCESSORIES = 7,  // accessory pages from the UI channel (can_channels.md)
};
struct ElcCanMenuInput {
  uint8_t event = 0;
  int8_t value = 0;
  uint8_t from_node = 0;
};
void elc_can_encode_menu_input(const ElcCanMenuInput &m, ElcCanFrame &out);
bool elc_can_decode_menu_input(const ElcCanFrame &f, ElcCanMenuInput &out);

static const uint8_t ELC_CAN_MENU_BY_TOUCH = 0;
static const uint8_t ELC_CAN_MENU_BY_CONSOLE = 1;
struct ElcCanMenuState {
  bool open = false;
  uint8_t screen = ELC_CAN_SCREEN_CENTER;  // which screen shows it
  uint8_t page = ELC_CAN_PAGE_HOME;
  uint8_t opened_by = ELC_CAN_MENU_BY_TOUCH;
  uint8_t text_serial = 0;  // 0..15, names the texts on 0x218 / 0x219 that go with this state
};
void elc_can_encode_menu_state(const ElcCanMenuState &s, ElcCanFrame &out);
bool elc_can_decode_menu_state(const ElcCanFrame &f, ElcCanMenuState &out);

// ---- wall time (center) ----
static const uint8_t ELC_CAN_TIME_VALID = 0x01;
static const uint8_t ELC_CAN_TIME_GPS = 0x02;  // from a GPS fix
// 0x04 reserved (was from the network, NTP)
struct ElcCanTime {
  uint32_t utc = 0;        // seconds since 1970
  int16_t offset_min = 0;  // local time = UTC + offset
  uint8_t flags = 0;       // byte 7 is reserved (was the place text serial), sent as 0
};
void elc_can_encode_time(const ElcCanTime &t, ElcCanFrame &out);
bool elc_can_decode_time(const ElcCanFrame &f, ElcCanTime &out);

// ---- phone (HFP calls, pairing) on the audio bridge ----
//
// The bridge sends PHONE_STATUS on change and every second, PHONE_TEXT for names and the caller, and PHONE_PAIRING
// while pairing. The console (or anyone) drives it with PHONE_COMMAND (0x215). Paired phones are addressed by their
// index in the bridge's list (most recently used first); a command that names an index carries the list serial it was
// read with, and the bridge ignores it when the list changed since.

static const uint8_t ELC_CAN_PHONE_CONNECTED = 0x01;   // a phone is connected (any profile)
static const uint8_t ELC_CAN_PHONE_HFP = 0x02;         // hands-free link up: calls possible
static const uint8_t ELC_CAN_PHONE_CALL_AUDIO = 0x04;  // call audio is in the car (SCO link up)
static const uint8_t ELC_CAN_PHONE_MIC_MUTED = 0x08;
static const uint8_t ELC_CAN_PHONE_PAIRING = 0x10;     // discoverable: a new phone can pair
static const uint8_t ELC_CAN_PHONE_CONFIRM = 0x20;     // a phone waits for the code to be confirmed (PHONE_PAIRING)
static const uint8_t ELC_CAN_PHONE_INBAND_RING = 0x40; // the phone plays its own ringtone through the car
static const uint8_t ELC_CAN_PHONE_VOICE = 0x80;       // voice assistant (Siri / Google) active

enum ElcCanCallState : uint8_t {
  ELC_CAN_CALL_IDLE = 0,
  ELC_CAN_CALL_INCOMING = 1,   // ringing
  ELC_CAN_CALL_DIALING = 2,    // outgoing, not ringing yet
  ELC_CAN_CALL_ALERTING = 3,   // outgoing, the other end rings
  ELC_CAN_CALL_ACTIVE = 4,
  ELC_CAN_CALL_WAITING = 5,    // active, and a second call is ringing
  ELC_CAN_CALL_HELD = 6,       // a call on hold (with or without an active one)
};

static const uint8_t ELC_CAN_PHONE_MAX_PAIRED = 8;
static const uint8_t ELC_CAN_PHONE_NONE = 0x0F;  // connected index / signal / battery: none or unknown

struct ElcCanPhoneStatus {
  uint8_t flags = 0;                           // ELC_CAN_PHONE_*
  uint8_t call = ELC_CAN_CALL_IDLE;            // ElcCanCallState
  uint16_t call_s = 0;                         // seconds since the call was answered
  uint8_t signal = ELC_CAN_PHONE_NONE;         // 0..5
  uint8_t battery = ELC_CAN_PHONE_NONE;        // 0..5
  uint8_t caller_serial = 0;                   // 0..15, serial of the caller text (kind 0)
  uint8_t list_serial = 0;                     // 0..15, bumps when the paired list changes
  uint8_t paired = 0;                          // phones in the list, 0..8
  uint8_t connected_index = ELC_CAN_PHONE_NONE;  // list index of the connected phone
  uint8_t call_volume = 0;                     // 0..15
};
void elc_can_encode_phone_status(uint8_t slot, const ElcCanPhoneStatus &s, ElcCanFrame &out);
bool elc_can_decode_phone_status(const ElcCanFrame &f, uint8_t *slot, ElcCanPhoneStatus &out);

/** What a PHONE_TEXT carries. Paired names use kind ELC_CAN_PHONE_TEXT_PAIRED + index, sent with the list serial. */
enum ElcCanPhoneTextKind : uint8_t {
  ELC_CAN_PHONE_TEXT_CALLER = 0,     // number (or name) of the call shown, sent with the caller serial
  ELC_CAN_PHONE_TEXT_CONNECTED = 1,  // name of the connected phone
  ELC_CAN_PHONE_TEXT_PAIRING = 2,    // name of the phone asking to pair
  ELC_CAN_PHONE_TEXT_PAIRED = 0x10,  // + index 0..7
};
static const size_t ELC_CAN_PHONE_TEXT_MAX = 32;
static const size_t ELC_CAN_PHONE_TEXT_CHUNKS = 6;  // 6 x 6 bytes >= 32
void elc_can_encode_phone_text(uint8_t slot, uint8_t kind, uint8_t serial, const char *text,
                               ElcCanFrame out[ELC_CAN_PHONE_TEXT_CHUNKS]);
/** Reassembles one kind of phone text. */
class ElcCanPhoneTextDecoder {
 public:
  explicit ElcCanPhoneTextDecoder(uint8_t kind = 0) : _kind(kind) {}
  void set_kind(uint8_t kind) { _kind = kind; _have = false; _serial = 0xFF; _seen = 0; }
  /** True when this frame completed a text (read it with text()). */
  bool feed(const ElcCanFrame &f);
  const char *text() const { return _done; }
  uint8_t serial() const { return _done_serial; }
  bool valid() const { return _have; }
  void clear() { _have = false; _done[0] = '\0'; _done_serial = 0xFF; }

 private:
  uint8_t _kind;
  char _buf[ELC_CAN_PHONE_TEXT_CHUNKS * 6 + 1] = {0};
  char _done[ELC_CAN_PHONE_TEXT_MAX + 1] = {0};
  uint8_t _serial = 0xFF, _done_serial = 0xFF, _seen = 0;
  bool _have = false;
};

/** PHONE_PAIRING: the code to compare (valid while ELC_CAN_PHONE_CONFIRM is set) and the pairing window. */
void elc_can_encode_phone_pairing(uint8_t slot, uint32_t passkey, uint8_t seconds_left, ElcCanFrame &out);
bool elc_can_decode_phone_pairing(const ElcCanFrame &f, uint8_t *slot, uint32_t *passkey, uint8_t *seconds_left);

enum ElcCanPhoneCommand : uint8_t {
  ELC_CAN_PHONE_CMD_ANSWER = 1,
  ELC_CAN_PHONE_CMD_END = 2,            // decline a ringing call, hang up, or cancel dialing
  ELC_CAN_PHONE_CMD_MIC_MUTE = 3,       // arg 1 mute, 0 unmute
  ELC_CAN_PHONE_CMD_CALL_VOLUME = 4,    // arg 0..15
  ELC_CAN_PHONE_CMD_PAIRING = 5,        // arg 1 open the pairing window, 0 close it
  ELC_CAN_PHONE_CMD_CONFIRM = 6,        // arg 1 accept the code, 0 reject
  ELC_CAN_PHONE_CMD_CONNECT = 7,        // arg = paired index (list serial checked)
  ELC_CAN_PHONE_CMD_DISCONNECT = 8,     // the connected phone
  ELC_CAN_PHONE_CMD_FORGET = 9,         // arg = paired index (list serial checked)
  ELC_CAN_PHONE_CMD_VOICE = 10,         // arg 1 start the voice assistant, 0 stop
  ELC_CAN_PHONE_CMD_REDIAL = 11,        // call the last number again
  ELC_CAN_PHONE_CMD_CALL_WAITING = 12,  // arg 0 decline the waiting call, 1 end active + answer, 2 hold active + answer / swap
  ELC_CAN_PHONE_CMD_AUDIO_ROUTE = 13,   // arg 1 call audio in the car, 0 on the phone
  ELC_CAN_PHONE_CMD_MEDIA = 14,         // arg 0 play / pause, 1 next, 2 previous, 3 play, 4 pause
  ELC_CAN_PHONE_CMD_MEDIA_VOLUME = 15,  // arg 0..15
};
struct ElcCanPhoneCommandMsg {
  uint8_t slot = 0;
  uint8_t command = 0;
  uint8_t arg = 0;
  uint8_t from_node = 0;
  uint8_t list_serial = 0;
};
void elc_can_encode_phone_command(const ElcCanPhoneCommandMsg &c, ElcCanFrame &out);
bool elc_can_decode_phone_command(const ElcCanFrame &f, ElcCanPhoneCommandMsg &out);

// ---- album art (JPEG from the phone), fetched over CAN on request ----
//
// A display asks with ART_REQUEST (slot, art id, first chunk, variant). The bridge answers with ART_INFO (size) and
// then ART_DATA chunks from that point to the end, paced (see the bridge). Frames are single-shot, so the receiver
// keeps a bitmap and asks again from the first missing chunk. Each variant is its own stream (the variant rides in
// the top bits of the chunk number), so a center and a console can fetch different ones at the same time.
// ART_INFO with size 0 means the bridge has no such variant for that art id.

enum ElcCanArtVariant : uint8_t {
  ELC_CAN_ART_ORIGINAL = 0,  // the phone's JPEG as received (typically 200x200)
  ELC_CAN_ART_JPEG_SMALL = 1,  // small JPEG (128x128; 64x64 without PSRAM), centre-cropped (fast first picture on the center)
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
