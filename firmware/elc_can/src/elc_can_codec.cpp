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
  frame(out, ELC_CAN_ID_CLUSTER_STATE, 2);
  out.data[0] = s.theme;
  out.data[1] = s.center_subview;
}

bool elc_can_decode_cluster_state(const ElcCanFrame &f, ElcCanClusterState &out) {
  if (f.id != ELC_CAN_ID_CLUSTER_STATE || f.len < 2) return false;
  out.theme = f.data[0];
  out.center_subview = f.data[1];
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
