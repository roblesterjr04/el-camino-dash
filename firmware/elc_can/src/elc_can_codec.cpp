#include "elc_can_codec.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void frame(ElcCanFrame &f, uint16_t id, uint8_t len) {
  f.id = id;
  f.len = len;
  memset(f.data, 0, sizeof(f.data));
}

void elc_can_encode_sensor(const ElcCanSensor &s, ElcCanFrame out[ELC_CAN_SENSOR_FRAMES]) {
  frame(out[0], ELC_CAN_ID_SENSOR_SPEED, 8);
  put32(out[0].data, s.speed_rpm);
  put32(out[0].data + 4, s.engine_rpm);

  frame(out[1], ELC_CAN_ID_SENSOR_ANALOG, 8);
  put16(out[1].data, s.pot_adc);
  put16(out[1].data + 2, s.therm_adc);
  put16(out[1].data + 4, s.pressure_adc);
  put16(out[1].data + 6, s.voltage_adc);

  frame(out[2], ELC_CAN_ID_SENSOR_STATUS, 6);
  put16(out[2].data, s.ambient_adc);
  out[2].data[2] = s.flags;
  put16(out[2].data + 3, s.faults);
  out[2].data[5] = s.seq;

  frame(out[3], ELC_CAN_ID_SENSOR_ODO, 8);
  put32(out[3].data, s.odometer_tenths_mile);
  put32(out[3].data + 4, s.dte_tenths_mile);

  frame(out[4], ELC_CAN_ID_SENSOR_ECON, 5);
  put16(out[4].data, s.mpg_instant_x10);
  put16(out[4].data + 2, s.mpg_average_x10);
  out[4].data[4] = s.has_fuel_economy ? 1 : 0;
}

bool ElcCanSensorDecoder::feed(const ElcCanFrame &f) {
  const uint8_t *d = f.data;
  switch (f.id) {
    case ELC_CAN_ID_SENSOR_SPEED:
      if (f.len < 8) return false;
      _v.speed_rpm = get32(d);
      _v.engine_rpm = get32(d + 4);
      _seen |= 1u << 0;
      return true;
    case ELC_CAN_ID_SENSOR_ANALOG:
      if (f.len < 8) return false;
      _v.pot_adc = get16(d);
      _v.therm_adc = get16(d + 2);
      _v.pressure_adc = get16(d + 4);
      _v.voltage_adc = get16(d + 6);
      _seen |= 1u << 1;
      return true;
    case ELC_CAN_ID_SENSOR_STATUS:
      if (f.len < 6) return false;
      _v.ambient_adc = get16(d);
      _v.flags = d[2];
      _v.faults = get16(d + 3);
      _v.seq = d[5];
      _seen |= 1u << 2;
      return true;
    case ELC_CAN_ID_SENSOR_ODO:
      if (f.len < 8) return false;
      _v.odometer_tenths_mile = get32(d);
      _v.dte_tenths_mile = get32(d + 4);
      _seen |= 1u << 3;
      return true;
    case ELC_CAN_ID_SENSOR_ECON:
      if (f.len < 5) return false;
      _v.mpg_instant_x10 = get16(d);
      _v.mpg_average_x10 = get16(d + 2);
      _v.has_fuel_economy = (d[4] & 1) != 0;
      _seen |= 1u << 4;
      return true;
    default:
      return false;
  }
}

void elc_can_encode_cluster_state(const ElcCanClusterState &s, ElcCanFrame &out) {
  frame(out, ELC_CAN_ID_CLUSTER_STATE, 6);
  out.data[0] = s.theme;
  out.data[1] = s.center_subview;
  out.data[2] = s.interface_screen;
  out.data[3] = s.media_screen;
  out.data[4] = s.brightness;
  out.data[5] = s.flags;
}

bool elc_can_decode_cluster_state(const ElcCanFrame &f, ElcCanClusterState &out) {
  if (f.id != ELC_CAN_ID_CLUSTER_STATE || f.len < 2) return false;
  out = ElcCanClusterState();
  out.theme = f.data[0];
  out.center_subview = f.data[1];
  if (f.len >= 6) {
    out.interface_screen = f.data[2] <= ELC_CAN_SCREEN_RIGHT ? f.data[2] : ELC_CAN_SCREEN_CENTER;
    out.media_screen = f.data[3] <= ELC_CAN_SCREEN_RIGHT ? f.data[3] : ELC_CAN_SCREEN_CENTER;
    out.brightness = f.data[4] < 10 ? 10 : (f.data[4] > 100 ? 100 : f.data[4]);
    out.flags = f.data[5];
  }
  return true;
}

void elc_can_encode_setting_request(const ElcCanSettingRequest &r, ElcCanFrame &out) {
  frame(out, ELC_CAN_ID_SETTING_REQUEST, 3);
  out.data[0] = r.setting;
  out.data[1] = r.value;
  out.data[2] = r.from_node;
}

bool elc_can_decode_setting_request(const ElcCanFrame &f, ElcCanSettingRequest &out) {
  if (f.id != ELC_CAN_ID_SETTING_REQUEST || f.len < 2) return false;
  out.setting = f.data[0];
  out.value = f.data[1];
  out.from_node = f.len >= 3 ? f.data[2] : 0;
  return true;
}

void elc_can_encode_center_status(const ElcCanCenterStatus &s, ElcCanFrame &out) {
  frame(out, ELC_CAN_ID_CENTER_STATUS, 3);
  out.data[0] = s.flags;
  out.data[1] = (uint8_t)s.rssi;
  out.data[2] = (uint8_t)(s.text_serial & 0x0F);
}

bool elc_can_decode_center_status(const ElcCanFrame &f, ElcCanCenterStatus &out) {
  if (f.id != ELC_CAN_ID_CENTER_STATUS || f.len < 3) return false;
  out.flags = f.data[0];
  out.rssi = (int8_t)f.data[1];
  out.text_serial = (uint8_t)(f.data[2] & 0x0F);
  return true;
}

void elc_can_encode_text(uint16_t id, uint8_t serial, const char *text, ElcCanFrame out[ELC_CAN_TEXT_CHUNKS]) {
  char buf[ELC_CAN_TEXT_CHUNKS * 7] = {0};
  if (text) {
    size_t n = strlen(text);
    if (n > ELC_CAN_TEXT_MAX) n = ELC_CAN_TEXT_MAX;
    memcpy(buf, text, n);
  }
  for (uint8_t c = 0; c < ELC_CAN_TEXT_CHUNKS; c++) {
    frame(out[c], id, 8);
    out[c].data[0] = (uint8_t)(((serial & 0x0F) << 4) | c);
    memcpy(out[c].data + 1, buf + 7 * c, 7);
  }
}

bool ElcCanTextDecoder::feed(const ElcCanFrame &f) {
  if (f.id != _id || f.len < 8) return false;
  const uint8_t serial = (uint8_t)(f.data[0] >> 4);
  const uint8_t chunk = (uint8_t)(f.data[0] & 0x0F);
  if (chunk >= ELC_CAN_TEXT_CHUNKS) return false;
  if (serial != _serial) {
    _serial = serial;
    _seen = 0;
    memset(_buf, 0, sizeof(_buf));
  }
  memcpy(_buf + 7 * chunk, f.data + 1, 7);
  _seen |= (uint8_t)(1u << chunk);
  if (_seen != (1u << ELC_CAN_TEXT_CHUNKS) - 1) return false;
  _seen = 0;
  memcpy(_done, _buf, ELC_CAN_TEXT_MAX);
  _done[ELC_CAN_TEXT_MAX] = '\0';
  _done_serial = serial;
  _have = true;
  return true;
}

void elc_can_encode_heartbeat(const ElcCanHeartbeat &h, ElcCanFrame &out) {
  frame(out, (uint16_t)(ELC_CAN_ID_HEARTBEAT_BASE + (h.node & 0x7F)), 8);
  out.data[0] = (uint8_t)h.type;
  put16(out.data + 1, h.fw_version);
  put32(out.data + 3, h.uptime_s);
  out.data[7] = h.flags;
}

bool elc_can_decode_heartbeat(const ElcCanFrame &f, ElcCanHeartbeat &out) {
  if (f.id < ELC_CAN_ID_HEARTBEAT_BASE || f.id > ELC_CAN_ID_HEARTBEAT_BASE + 0x7F || f.len < 8) return false;
  out.node = (uint8_t)(f.id - ELC_CAN_ID_HEARTBEAT_BASE);
  out.type = (ElcCanNodeType)f.data[0];
  out.fw_version = get16(f.data + 1);
  out.uptime_s = get32(f.data + 3);
  out.flags = f.data[7];
  return true;
}

bool ElcCanBridgeSeen::feed(const ElcCanFrame &f, uint32_t now_ms) {
  ElcCanHeartbeat hb;
  if (!elc_can_decode_heartbeat(f, hb)) return false;
  if (hb.node >= ELC_CAN_NODE_SLOT0 && hb.node < ELC_CAN_NODE_SLOT0 + ELC_CAN_RANGE_COUNT) {
    const int s = hb.node - ELC_CAN_NODE_SLOT0;
    _bridge[s] = hb.type == ElcCanNodeType::AudioBridge;
    _ms[s] = now_ms;
  }
  return true;
}

bool ElcCanBridgeSeen::ok(int slot, uint32_t now_ms) const {
  if (slot < 0 || slot >= ELC_CAN_RANGE_COUNT || slot == ELC_CAN_RANGE_CONSOLE || !_bridge[slot]) return false;
  return now_ms - _ms[slot] <= kMaxAgeMs;
}

void elc_can_encode_audio_transport(uint8_t slot, const ElcCanAudioTransport &t, ElcCanFrame &out) {
  frame(out, elc_can_slot_id(slot, ELC_CAN_AUDIO_MSG_TRANSPORT), 8);
  put16(out.data, t.elapsed_s);
  put16(out.data + 2, t.duration_s);
  out.data[4] = t.volume;
  out.data[5] = (uint8_t)(t.text_serial & 0x0F);
  out.data[6] = t.connected ? 1 : 0;
  out.data[7] = t.art_id;
}

bool elc_can_decode_audio_transport(const ElcCanFrame &f, uint8_t *slot, ElcCanAudioTransport &out) {
  const int s = elc_can_slot_of(f.id);
  if (s < 0 || f.id != elc_can_slot_id((uint8_t)s, ELC_CAN_AUDIO_MSG_TRANSPORT) || f.len < 7) return false;
  if (slot) *slot = (uint8_t)s;
  out.elapsed_s = get16(f.data);
  out.duration_s = get16(f.data + 2);
  out.volume = f.data[4];
  out.text_serial = (uint8_t)(f.data[5] & 0x0F);
  out.connected = (f.data[6] & 1) != 0;
  out.art_id = f.len >= 8 ? f.data[7] : 0;
  return true;
}

void elc_can_encode_audio_text(uint8_t slot, uint8_t serial, const char *text,
                               ElcCanFrame out[ELC_CAN_AUDIO_TEXT_CHUNKS]) {
  char buf[ELC_CAN_AUDIO_TEXT_CHUNKS * 7] = {0};
  if (text) {
    size_t n = strlen(text);
    if (n > ELC_CAN_AUDIO_TEXT_MAX) n = ELC_CAN_AUDIO_TEXT_MAX;
    memcpy(buf, text, n);
  }
  for (uint8_t c = 0; c < ELC_CAN_AUDIO_TEXT_CHUNKS; c++) {
    frame(out[c], elc_can_slot_id(slot, ELC_CAN_AUDIO_MSG_TEXT), 8);
    out[c].data[0] = (uint8_t)(((serial & 0x0F) << 4) | c);
    memcpy(out[c].data + 1, buf + 7 * c, 7);
  }
}

bool ElcCanAudioTextDecoder::feed(const ElcCanFrame &f) {
  const int s = elc_can_slot_of(f.id);
  if (s < 0 || f.id != elc_can_slot_id((uint8_t)s, ELC_CAN_AUDIO_MSG_TEXT) || f.len < 8) return false;
  const uint8_t serial = (uint8_t)(f.data[0] >> 4);
  const uint8_t chunk = (uint8_t)(f.data[0] & 0x0F);
  if (chunk >= ELC_CAN_AUDIO_TEXT_CHUNKS) return false;
  if (serial != _serial) {
    _serial = serial;
    _seen = 0;
    memset(_buf, 0, sizeof(_buf));
  }
  memcpy(_buf + 7 * chunk, f.data + 1, 7);
  _seen |= (uint8_t)(1u << chunk);
  if (_seen != (1u << ELC_CAN_AUDIO_TEXT_CHUNKS) - 1) return false;
  _seen = 0;  // the sender repeats the text; accept the next complete copy too
  memcpy(_done, _buf, ELC_CAN_AUDIO_TEXT_MAX);
  _done[ELC_CAN_AUDIO_TEXT_MAX] = '\0';
  _done_serial = serial;
  _have = true;
  return true;
}

// ---- console menu input / state ----

void elc_can_encode_menu_input(const ElcCanMenuInput &m, ElcCanFrame &out) {
  frame(out, ELC_CAN_ID_MENU_INPUT, 3);
  out.data[0] = m.event;
  out.data[1] = (uint8_t)m.value;
  out.data[2] = m.from_node;
}

bool elc_can_decode_menu_input(const ElcCanFrame &f, ElcCanMenuInput &out) {
  if (f.id != ELC_CAN_ID_MENU_INPUT || f.len < 3) return false;
  out.event = f.data[0];
  out.value = (int8_t)f.data[1];
  out.from_node = f.data[2];
  return true;
}

void elc_can_encode_menu_state(const ElcCanMenuState &s, ElcCanFrame &out) {
  frame(out, ELC_CAN_ID_MENU_STATE, 5);
  out.data[0] = s.open ? 1 : 0;
  out.data[1] = s.screen;
  out.data[2] = s.page;
  out.data[3] = s.opened_by;
  out.data[4] = (uint8_t)(s.text_serial & 0x0F);
}

bool elc_can_decode_menu_state(const ElcCanFrame &f, ElcCanMenuState &out) {
  if (f.id != ELC_CAN_ID_MENU_STATE || f.len < 5) return false;
  out.open = (f.data[0] & 1) != 0;
  out.screen = f.data[1] <= ELC_CAN_SCREEN_RIGHT ? f.data[1] : ELC_CAN_SCREEN_CENTER;
  out.page = f.data[2];
  out.opened_by = f.data[3];
  out.text_serial = (uint8_t)(f.data[4] & 0x0F);
  return true;
}

// ---- time ----

void elc_can_encode_time(const ElcCanTime &t, ElcCanFrame &out) {
  frame(out, ELC_CAN_ID_TIME, 8);
  put32(out.data, t.utc);
  put16(out.data + 4, (uint16_t)t.offset_min);
  out.data[6] = t.flags;
  out.data[7] = (uint8_t)(t.place_serial & 0x0F);
}

bool elc_can_decode_time(const ElcCanFrame &f, ElcCanTime &out) {
  if (f.id != ELC_CAN_ID_TIME || f.len < 8) return false;
  out.utc = get32(f.data);
  out.offset_min = (int16_t)get16(f.data + 4);
  out.flags = f.data[6];
  out.place_serial = (uint8_t)(f.data[7] & 0x0F);
  return true;
}

// ---- phone ----

void elc_can_encode_phone_status(uint8_t slot, const ElcCanPhoneStatus &s, ElcCanFrame &out) {
  frame(out, elc_can_slot_id(slot, ELC_CAN_AUDIO_MSG_PHONE_STATUS), 8);
  out.data[0] = s.flags;
  out.data[1] = s.call;
  put16(out.data + 2, s.call_s);
  out.data[4] = (uint8_t)((s.signal & 0x0F) | ((s.battery & 0x0F) << 4));
  out.data[5] = (uint8_t)((s.caller_serial & 0x0F) | ((s.list_serial & 0x0F) << 4));
  out.data[6] = (uint8_t)((s.paired & 0x0F) | ((s.connected_index & 0x0F) << 4));
  out.data[7] = s.call_volume;
}

bool elc_can_decode_phone_status(const ElcCanFrame &f, uint8_t *slot, ElcCanPhoneStatus &out) {
  const int s = elc_can_slot_of(f.id);
  if (s < 0 || f.id != elc_can_slot_id((uint8_t)s, ELC_CAN_AUDIO_MSG_PHONE_STATUS) || f.len < 8) return false;
  if (slot) *slot = (uint8_t)s;
  out.flags = f.data[0];
  out.call = f.data[1];
  out.call_s = get16(f.data + 2);
  out.signal = (uint8_t)(f.data[4] & 0x0F);
  out.battery = (uint8_t)(f.data[4] >> 4);
  out.caller_serial = (uint8_t)(f.data[5] & 0x0F);
  out.list_serial = (uint8_t)(f.data[5] >> 4);
  out.paired = (uint8_t)(f.data[6] & 0x0F);
  out.connected_index = (uint8_t)(f.data[6] >> 4);
  out.call_volume = f.data[7];
  return true;
}

void elc_can_encode_phone_text(uint8_t slot, uint8_t kind, uint8_t serial, const char *text,
                               ElcCanFrame out[ELC_CAN_PHONE_TEXT_CHUNKS]) {
  char buf[ELC_CAN_PHONE_TEXT_CHUNKS * 6] = {0};
  if (text) {
    size_t n = strlen(text);
    if (n > ELC_CAN_PHONE_TEXT_MAX) n = ELC_CAN_PHONE_TEXT_MAX;
    memcpy(buf, text, n);
  }
  for (uint8_t c = 0; c < ELC_CAN_PHONE_TEXT_CHUNKS; c++) {
    frame(out[c], elc_can_slot_id(slot, ELC_CAN_AUDIO_MSG_PHONE_TEXT), 8);
    out[c].data[0] = kind;
    out[c].data[1] = (uint8_t)(((serial & 0x0F) << 4) | c);
    memcpy(out[c].data + 2, buf + 6 * c, 6);
  }
}

bool ElcCanPhoneTextDecoder::feed(const ElcCanFrame &f) {
  const int s = elc_can_slot_of(f.id);
  if (s < 0 || f.id != elc_can_slot_id((uint8_t)s, ELC_CAN_AUDIO_MSG_PHONE_TEXT) || f.len < 8) return false;
  if (f.data[0] != _kind) return false;
  const uint8_t serial = (uint8_t)(f.data[1] >> 4);
  const uint8_t chunk = (uint8_t)(f.data[1] & 0x0F);
  if (chunk >= ELC_CAN_PHONE_TEXT_CHUNKS) return false;
  if (serial != _serial) {
    _serial = serial;
    _seen = 0;
    memset(_buf, 0, sizeof(_buf));
  }
  memcpy(_buf + 6 * chunk, f.data + 2, 6);
  _seen |= (uint8_t)(1u << chunk);
  if (_seen != (1u << ELC_CAN_PHONE_TEXT_CHUNKS) - 1) return false;
  _seen = 0;  // the sender repeats; accept the next complete copy too
  memcpy(_done, _buf, ELC_CAN_PHONE_TEXT_MAX);
  _done[ELC_CAN_PHONE_TEXT_MAX] = '\0';
  _done_serial = serial;
  _have = true;
  return true;
}

void elc_can_encode_phone_pairing(uint8_t slot, uint32_t passkey, uint8_t seconds_left, ElcCanFrame &out) {
  frame(out, elc_can_slot_id(slot, ELC_CAN_AUDIO_MSG_PHONE_PAIRING), 5);
  put32(out.data, passkey);
  out.data[4] = seconds_left;
}

bool elc_can_decode_phone_pairing(const ElcCanFrame &f, uint8_t *slot, uint32_t *passkey, uint8_t *seconds_left) {
  const int s = elc_can_slot_of(f.id);
  if (s < 0 || f.id != elc_can_slot_id((uint8_t)s, ELC_CAN_AUDIO_MSG_PHONE_PAIRING) || f.len < 5) return false;
  if (slot) *slot = (uint8_t)s;
  if (passkey) *passkey = get32(f.data);
  if (seconds_left) *seconds_left = f.data[4];
  return true;
}

void elc_can_encode_phone_command(const ElcCanPhoneCommandMsg &c, ElcCanFrame &out) {
  frame(out, ELC_CAN_ID_PHONE_COMMAND, 5);
  out.data[0] = c.slot;
  out.data[1] = c.command;
  out.data[2] = c.arg;
  out.data[3] = c.from_node;
  out.data[4] = c.list_serial;
}

bool elc_can_decode_phone_command(const ElcCanFrame &f, ElcCanPhoneCommandMsg &out) {
  if (f.id != ELC_CAN_ID_PHONE_COMMAND || f.len < 5) return false;
  out.slot = f.data[0];
  out.command = f.data[1];
  out.arg = f.data[2];
  out.from_node = f.data[3];
  out.list_serial = f.data[4];
  return true;
}

// ---- album art ----

void elc_can_encode_art_request(const ElcCanArtRequest &r, ElcCanFrame &out) {
  frame(out, ELC_CAN_ID_AUDIO_ART_REQUEST, 5);
  out.data[0] = r.slot;
  out.data[1] = r.art_id;
  put16(out.data + 2, r.first_chunk);
  out.data[4] = r.variant;
}

bool elc_can_decode_art_request(const ElcCanFrame &f, ElcCanArtRequest &out) {
  if (f.id != ELC_CAN_ID_AUDIO_ART_REQUEST || f.len < 4) return false;
  out.slot = f.data[0];
  out.art_id = f.data[1];
  out.first_chunk = get16(f.data + 2);
  out.variant = f.len >= 5 ? f.data[4] : (uint8_t)ELC_CAN_ART_ORIGINAL;
  return true;
}

void elc_can_encode_art_info(uint8_t slot, uint8_t art_id, uint8_t variant, uint32_t size, ElcCanFrame &out) {
  frame(out, elc_can_slot_id(slot, ELC_CAN_AUDIO_MSG_ART_INFO), 6);
  out.data[0] = art_id;
  put32(out.data + 1, size);
  out.data[5] = variant;
}

bool elc_can_decode_art_info(const ElcCanFrame &f, uint8_t *slot, uint8_t *art_id, uint8_t *variant, uint32_t *size) {
  const int s = elc_can_slot_of(f.id);
  if (s < 0 || f.id != elc_can_slot_id((uint8_t)s, ELC_CAN_AUDIO_MSG_ART_INFO) || f.len < 5) return false;
  if (slot) *slot = (uint8_t)s;
  if (art_id) *art_id = f.data[0];
  if (size) *size = get32(f.data + 1);
  if (variant) *variant = f.len >= 6 ? f.data[5] : (uint8_t)ELC_CAN_ART_ORIGINAL;
  return true;
}

void elc_can_encode_art_data(uint8_t slot, uint8_t art_id, uint8_t variant, uint16_t idx, const uint8_t *image,
                             uint32_t size, ElcCanFrame &out) {
  const uint32_t off = (uint32_t)(idx & 0x3FFF) * ELC_CAN_ART_CHUNK;
  const uint32_t n = off >= size ? 0 : (size - off < ELC_CAN_ART_CHUNK ? size - off : ELC_CAN_ART_CHUNK);
  frame(out, elc_can_slot_id(slot, ELC_CAN_AUDIO_MSG_ART_DATA), (uint8_t)(3 + n));
  out.data[0] = art_id;
  put16(out.data + 1, (uint16_t)(((variant & 3) << 14) | (idx & 0x3FFF)));
  if (n) memcpy(out.data + 3, image + off, n);
}

void ElcCanArtReceiver::start(uint8_t slot, uint8_t art_id, uint8_t variant, uint8_t *buf, size_t cap) {
  _buf = buf;
  _cap = cap;
  _slot = slot;
  _id = art_id;
  _variant = variant;
  _size = 0;
  _have = 0;
  _too_big = false;
  memset(_bits, 0, sizeof(_bits));
}

bool ElcCanArtReceiver::feed(const ElcCanFrame &f) {
  if (_id == 0 || _too_big) return false;
  const int s = elc_can_slot_of(f.id);
  if (s != (int)_slot) return false;
  uint8_t id = 0, variant = 0;
  uint32_t size = 0;
  if (elc_can_decode_art_info(f, nullptr, &id, &variant, &size)) {
    if (id != _id || variant != _variant) return false;
    if (_size == 0) {
      if (size == 0 || size > _cap || size > ELC_CAN_ART_MAX_BYTES || !_buf) {
        _too_big = true;
      } else {
        _size = size;
      }
    }
    return true;
  }
  if (f.id != elc_can_slot_id(_slot, ELC_CAN_AUDIO_MSG_ART_DATA) || f.len < 4 || f.data[0] != _id) return false;
  const uint16_t field = get16(f.data + 1);
  if ((field >> 14) != _variant) return false;  // another display's stream
  if (_size == 0) return true;  // ours, but the size is not known yet: it will be asked for again
  const uint16_t idx = field & 0x3FFF;
  if (idx >= elc_can_art_chunks(_size)) return true;
  const uint32_t off = (uint32_t)idx * ELC_CAN_ART_CHUNK;
  const uint32_t want = _size - off < ELC_CAN_ART_CHUNK ? _size - off : ELC_CAN_ART_CHUNK;
  if ((uint32_t)(f.len - 3) != want) return true;  // malformed: leave the chunk missing
  if (!(_bits[idx >> 3] & (1u << (idx & 7)))) {
    memcpy(_buf + off, f.data + 3, want);
    _bits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    _have++;
  }
  return true;
}

uint16_t ElcCanArtReceiver::first_missing() const {
  const uint16_t n = _size ? elc_can_art_chunks(_size) : 0;
  for (uint16_t i = 0; i < n; i++) {
    if (!(_bits[i >> 3] & (1u << (i & 7)))) return i;
  }
  return n;
}
