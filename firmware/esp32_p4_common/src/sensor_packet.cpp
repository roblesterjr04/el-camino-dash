#include <sensor_packet.h>

#include <cmath>
#include <cstring>

namespace {

constexpr size_t kUartTextQ = 8;
/** Long enough for sensor_pack ODO_FRAM* lines (e.g. "ODO_FRAM read OK v2 tenths=999999"). */
constexpr size_t kUartTextMax = 96;
char s_uart_text_q[kUartTextQ][kUartTextMax];
uint8_t s_uart_text_q_head = 0;
uint8_t s_uart_text_q_count = 0;

void uart_text_q_push(const char *s) {
  size_t n = strlen(s);
  if (n >= kUartTextMax) {
    n = kUartTextMax - 1;
  }
  if (s_uart_text_q_count >= kUartTextQ) {
    s_uart_text_q_head = (uint8_t)((s_uart_text_q_head + 1) % kUartTextQ);
    s_uart_text_q_count--;
  }
  uint8_t slot = (uint8_t)((s_uart_text_q_head + s_uart_text_q_count) % kUartTextQ);
  memcpy(s_uart_text_q[slot], s, n);
  s_uart_text_q[slot][n] = 0;
  s_uart_text_q_count++;
}

}  // namespace

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

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint8_t SensorPacketParser::xor_payload(const uint8_t *p, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
  }
  return c;
}

bool SensorPacketParser::parse_payload(const uint8_t *payload, size_t len, SensorPacketValues &out) {
  if (len != ECONOMY_PAYLOAD_LEN) {
    return false;
  }
  out.pot_adc = (uint16_t)((payload[0] << 8) | payload[1]);
  out.therm_adc = (uint16_t)((payload[2] << 8) | payload[3]);
  out.pressure_adc = (uint16_t)((payload[4] << 8) | payload[5]);
  out.voltage_adc = (uint16_t)((payload[6] << 8) | payload[7]);
  out.ambient_adc = (uint16_t)((payload[8] << 8) | payload[9]);
  out.speed_rpm = be32(payload + 10);
  out.engine_rpm = be32(payload + 14);
  out.flags = payload[18];
  out.faults = (uint16_t)((payload[19] << 8) | payload[20]);
  out.odometer_tenths_mile = be32(payload + 21);
  // 0xFFFFFFFF = sensor_pack could not read its FRAM (odometer unknown).
  out.has_odometer = out.odometer_tenths_mile != ODOMETER_UNKNOWN;
  if (!out.has_odometer) {
    out.odometer_tenths_mile = 0;
  }
  out.mpg_instant_x10 = (uint16_t)((payload[25] << 8) | payload[26]);
  out.mpg_average_x10 = (uint16_t)((payload[27] << 8) | payload[28]);
  out.dte_tenths_mile = be32(payload + 29);
  out.has_fuel_economy = true;
  out.extended = true;
  out.valid = true;
  return true;
}

/** Index of the first AA 55 at or after `from`, or _len if none. */
size_t SensorPacketParser::find_sync(size_t from) const {
  for (size_t i = from; i + 1 < _len; i++) {
    if (_buf[i] == SENSOR_SYNC0 && _buf[i + 1] == SENSOR_SYNC1) {
      return i;
    }
  }
  return _len;
}

void SensorPacketParser::consume(size_t n) {
  if (n >= _len) {
    _len = 0;
    return;
  }
  memmove(_buf, _buf + n, _len - n);
  _len -= n;
}

/**
 * Consume complete text lines (sensor_pack boot banner, FRAM status) at the front of the buffer.
 * Stops at a frame sync. Bytes before a sync that are not a newline-terminated line are dropped as
 * noise — a 0x0A inside a following binary frame can no longer swallow that frame.
 */
void SensorPacketParser::stripLeadingUartTextLines() {
  while (_len > 0) {
    if (_buf[0] == SENSOR_SYNC0 && (_len == 1 || _buf[1] == SENSOR_SYNC1)) {
      return;  // frame (or possible frame start) at the front
    }
    size_t nl = 0;
    while (nl < _len && _buf[nl] != '\n') {
      nl++;
    }
    const size_t sync = find_sync(1);
    if (sync < nl) {
      _skipped += sync;
      consume(sync);  // unterminated junk before a frame
      continue;
    }
    if (nl >= _len) {
      return;  // partial line: wait for more bytes
    }
    size_t ll = nl;
    if (ll > 0 && _buf[ll - 1] == '\r') {
      ll--;
    }
    bool printable = ll > 0;
    for (size_t i = 0; i < ll; i++) {
      unsigned char c = _buf[i];
      if (c < 0x20 || c > 0x7e) {
        printable = false;
        break;
      }
    }
    if (printable) {
      char tmp[kUartTextMax];
      size_t copy = ll;
      if (copy >= kUartTextMax) {
        copy = kUartTextMax - 1;
      }
      memcpy(tmp, _buf, copy);
      tmp[copy] = 0;
      uart_text_q_push(tmp);
    }
    consume(nl + 1);
  }
}

bool SensorPacketParser::takePendingTextLine(char *out, size_t cap) {
  if (!out || cap == 0) {
    return false;
  }
  if (s_uart_text_q_count == 0) {
    return false;
  }
  const char *src = s_uart_text_q[s_uart_text_q_head];
  size_t i = 0;
  for (; src[i] && i + 1 < cap; i++) {
    out[i] = src[i];
  }
  out[i] = 0;
  s_uart_text_q_head = (uint8_t)((s_uart_text_q_head + 1) % kUartTextQ);
  s_uart_text_q_count--;
  return true;
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
      consume(_len - 64);  // overflow: keep the tail and resync
    }
    _buf[_len++] = (uint8_t)c;
  }

  // Returns after the first good frame so callers can loop; leftover bytes stay buffered.
  constexpr size_t kFrameLen = 2 + ECONOMY_PAYLOAD_LEN + 1;
  for (;;) {
    stripLeadingUartTextLines();
    if (_len < 2) {
      return false;
    }
    if (_buf[0] != SENSOR_SYNC0 || _buf[1] != SENSOR_SYNC1) {
      // Partial text line or junk with no sync yet: wait, unless it can never become a line.
      if (_len > kUartTextMax + kFrameLen) {
        consume(1);
        continue;
      }
      return false;
    }
    // Only full v4 frames are accepted: waiting for all 36 bytes means a frame that is still
    // arriving can no longer be misread as an older, shorter layout.
    if (_len < kFrameLen) {
      return false;
    }
    const uint8_t *payload = _buf + 2;
    if (xor_payload(payload, ECONOMY_PAYLOAD_LEN) == _buf[2 + ECONOMY_PAYLOAD_LEN]) {
      SensorPacketValues tmp{};
      if (parse_payload(payload, ECONOMY_PAYLOAD_LEN, tmp)) {
        consume(kFrameLen);
        _ok++;
        out = tmp;
        if (send_handshake_on_good) {
          _ser->write(PI_HANDSHAKE_BYTE);
        }
        return true;
      }
    }
    _bad++;
    _skipped++;
    consume(1);  // bad candidate: skip this sync byte and rescan
  }
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
