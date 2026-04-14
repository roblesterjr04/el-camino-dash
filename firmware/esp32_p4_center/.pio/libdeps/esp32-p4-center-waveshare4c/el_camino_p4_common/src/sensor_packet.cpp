#include <sensor_packet.h>

#include <cmath>

static float map_adc(uint16_t raw, float in_lo, float in_hi, float out_lo, float out_hi) {
  float r = (float)raw;
  if (in_hi <= in_lo) {
    return out_lo;
  }
  float t = (r - in_lo) / (in_hi - in_lo);
  if (t < 0.0f) {
    t = 0.0f;
  }
  if (t > 1.0f) {
    t = 1.0f;
  }
  return out_lo + (out_hi - out_lo) * t;
}

// 100k NTC, B3950 — same as arduino_serial._thermistor_temp_f
static float thermistor_temp_f(uint16_t raw_adc) {
  if (raw_adc <= 0 || raw_adc >= 1023) {
    return 100.0f;
  }
  const float therm_series_r = 100000.0f;
  const float therm_nominal_r = 100000.0f;
  const float therm_nominal_t_c = 25.0f;
  const float therm_beta = 3950.0f;

  float resistance = therm_series_r * ((1023.0f / (float)raw_adc) - 1.0f);
  if (resistance <= 0.0f) {
    return 100.0f;
  }
  float steinhart = logf(resistance / therm_nominal_r) / therm_beta;
  steinhart += 1.0f / (therm_nominal_t_c + 273.15f);
  float temp_c = (1.0f / steinhart) - 273.15f;
  if (!std::isfinite(temp_c)) {
    return 100.0f;
  }
  return (temp_c * 9.0f / 5.0f) + 32.0f;
}

uint8_t SensorPacketParser::xor_payload(const uint8_t *p, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
  }
  return c;
}

bool SensorPacketParser::parse_payload(const uint8_t *payload, size_t len, SensorPacketValues &out) {
  if (len == LEGACY_PAYLOAD_LEN) {
    out.pot_adc = (uint16_t)((payload[0] << 8) | payload[1]);
    out.therm_adc = (uint16_t)((payload[2] << 8) | payload[3]);
    out.pressure_adc = (uint16_t)((payload[4] << 8) | payload[5]);
    out.voltage_adc = (uint16_t)((payload[6] << 8) | payload[7]);
    out.ambient_adc = 0;
    out.speed_rpm = ((uint32_t)payload[8] << 24) | ((uint32_t)payload[9] << 16) | ((uint32_t)payload[10] << 8) | payload[11];
    out.engine_rpm = ((uint32_t)payload[12] << 24) | ((uint32_t)payload[13] << 16) | ((uint32_t)payload[14] << 8) | payload[15];
    out.flags = payload[16];
    out.faults = (uint16_t)((payload[17] << 8) | payload[18]);
    out.extended = false;
    out.valid = true;
    return true;
  }
  if (len == EXTENDED_PAYLOAD_LEN) {
    out.pot_adc = (uint16_t)((payload[0] << 8) | payload[1]);
    out.therm_adc = (uint16_t)((payload[2] << 8) | payload[3]);
    out.pressure_adc = (uint16_t)((payload[4] << 8) | payload[5]);
    out.voltage_adc = (uint16_t)((payload[6] << 8) | payload[7]);
    out.ambient_adc = (uint16_t)((payload[8] << 8) | payload[9]);
    out.speed_rpm = ((uint32_t)payload[10] << 24) | ((uint32_t)payload[11] << 16) | ((uint32_t)payload[12] << 8) | payload[13];
    out.engine_rpm = ((uint32_t)payload[14] << 24) | ((uint32_t)payload[15] << 16) | ((uint32_t)payload[16] << 8) | payload[17];
    out.flags = payload[18];
    out.faults = (uint16_t)((payload[19] << 8) | payload[20]);
    out.extended = true;
    out.valid = true;
    return true;
  }
  return false;
}

bool SensorPacketParser::poll(SensorPacketValues &out, bool send_handshake_on_good) {
  out.valid = false;
  if (!_ser) {
    return false;
  }
  while (_ser->available()) {
    int c = _ser->read();
    if (c < 0) {
      break;
    }
    if (_len >= sizeof(_buf)) {
      _len = 0;
    }
    _buf[_len++] = (uint8_t)c;
  }

  bool got = false;
  while (_len >= (2 + LEGACY_PAYLOAD_LEN + 1)) {
    size_t sync_idx = 0;
    while (sync_idx + 1 < _len && (_buf[sync_idx] != SENSOR_SYNC0 || _buf[sync_idx + 1] != SENSOR_SYNC1)) {
      sync_idx++;
    }
    if (sync_idx > 0) {
      memmove(_buf, _buf + sync_idx, _len - sync_idx);
      _len -= sync_idx;
    }
    if (_len < (2 + LEGACY_PAYLOAD_LEN + 1)) {
      break;
    }

    bool frame_ok = false;
    for (size_t plen : {EXTENDED_PAYLOAD_LEN, LEGACY_PAYLOAD_LEN}) {
      size_t frame_len = 2 + plen + 1;
      if (_len < frame_len) {
        continue;
      }
      const uint8_t *payload = _buf + 2;
      uint8_t crc_rx = _buf[2 + plen];
      if (xor_payload(payload, plen) != crc_rx) {
        continue;
      }
      SensorPacketValues tmp;
      if (!parse_payload(payload, plen, tmp)) {
        continue;
      }
      out = tmp;
      got = true;
      frame_ok = true;
      memmove(_buf, _buf + frame_len, _len - frame_len);
      _len -= frame_len;
      break;
    }
    if (frame_ok) {
      break;
    }
    // Bad sync candidate — drop one byte and rescan (matches Python).
    memmove(_buf, _buf + 1, _len - 1);
    _len--;
  }

  if (got && send_handshake_on_good) {
    _ser->write(PI_HANDSHAKE_BYTE);
  }
  return got;
}

SensorDerived SensorPacketParser::derive(const SensorPacketValues &p) {
  SensorDerived d{};
  d.valid = p.valid;
  if (!p.valid) {
    return d;
  }
  // Default ranges from arduino_serial defaults
  d.fuel_pct = map_adc(p.pot_adc, 0.0f, 1023.0f, 0.0f, 100.0f);
  d.oil_psi = map_adc(p.pressure_adc, 0.0f, 1023.0f, 0.0f, 100.0f);
  d.coolant_temp_f = thermistor_temp_f(p.therm_adc);
  // Map ADC to -100..+100 (same linear mapping as 10–16 V on the Pi gauge config)
  d.system_amp = map_adc(p.voltage_adc, 0.0f, 1023.0f, -100.0f, 100.0f);
  return d;
}
