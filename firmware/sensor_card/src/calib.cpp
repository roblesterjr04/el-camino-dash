#include "calib.h"

#include <math.h>
#include <string.h>

#include "elc_chan.h"  // elc_ch_crc16

namespace cal {

void defaults(Calibration &c) {
  c = Calibration();
  c.fuel.bias_x10 = 1000;   // JP6: 100 ohm
  c.fuel.preset = 1;        // the factory GM 0-30 ohm sender
  c.temp.bias_x10 = 10000;  // JP10: 1k, no preset: not calibrated
  c.oil.bias_x10 = 2200;    // JP12: 220 ohm, no preset: not calibrated
}

ChannelCal &channel(Calibration &c, uint8_t ch) { return ch == FUEL ? c.fuel : ch == TEMP ? c.temp : c.oil; }
const ChannelCal &channel(const Calibration &c, uint8_t ch) {
  return ch == FUEL ? c.fuel : ch == TEMP ? c.temp : c.oil;
}

float raw_to_ohms(const ChannelCal &c, float raw) {
  if (raw < 0.5f) raw = 0.5f;  // a saturated reading is a very small (or very large) resistance, not zero
  if (raw > 1022.5f) raw = 1022.5f;
  const float bias = (float)c.bias_x10 / 10.0f;
  return c.topology == PULL_UP ? bias * raw / (1023.0f - raw) : bias * (1023.0f - raw) / raw;
}

float ohms_to_raw(const ChannelCal &c, float ohms) {
  const float bias = (float)c.bias_x10 / 10.0f;
  if (ohms < 0.0f) ohms = 0.0f;
  return c.topology == PULL_UP ? 1023.0f * ohms / (ohms + bias) : 1023.0f * bias / (ohms + bias);
}

// ---- sender curves

float ntc_celsius(float ohms, float r0) {
  if (ohms <= 0.0f) return 300.0f;
  return 1.0f / (1.0f / 298.15f + logf(ohms / r0) / 3950.0f) - 273.15f;
}

// GM's resistance chart for the 2-wire coolant sensor (degF -> ohms): entered from the published chart and to be checked
// against a captured point on the real sender before it is trusted (sensor_calibration.md, Presets).
static const float kGmF[] = {-40.0f, 0.0f, 20.0f, 40.0f, 70.0f, 100.0f, 160.0f, 210.0f};
static const float kGmOhms[] = {100700.0f, 25000.0f, 13500.0f, 7500.0f, 3400.0f, 1800.0f, 450.0f, 185.0f};
static const int kGmN = 8;

float gm_coolant_celsius(float ohms) {
  if (ohms <= 0.0f) ohms = 1.0f;
  const float lr = logf(ohms);
  int i = 0;  // the segment [i, i+1]; beyond the ends the end segment is extended
  while (i < kGmN - 2 && ohms < kGmOhms[i + 1]) i++;
  const float l0 = logf(kGmOhms[i]), l1 = logf(kGmOhms[i + 1]);
  const float f = kGmF[i] + (kGmF[i + 1] - kGmF[i]) * (lr - l0) / (l1 - l0);
  return (f - 32.0f) * 5.0f / 9.0f;
}

struct FuelPreset {
  float empty, full;  // sender ohms
};
static const FuelPreset kFuel[kFuelPresets] = {
    {0, 0}, {0, 30}, {0, 90}, {73, 10}, {240, 33}, {10, 180},
};

float fuel_fraction(uint8_t preset, float ohms) {
  if (preset == 0 || preset >= kFuelPresets) return 0.0f;
  const FuelPreset &p = kFuel[preset];
  float f = (ohms - p.empty) / (p.full - p.empty);
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  return f;
}

// ---- conversion

bool calibrated(const Calibration &c, uint8_t ch) {
  const ChannelCal &k = channel(c, ch);
  return k.n >= 2 || k.preset != 0;
}

/** The preset curve's value (before any offset or one-point shift) at a sender resistance. */
static float preset_value(const Calibration &c, uint8_t ch, const ChannelCal &k, float ohms) {
  if (ch == FUEL) return fuel_fraction(k.preset, ohms) * (float)c.tank_x10;
  switch (k.preset) {
    case 1: return ntc_celsius(ohms, 100000.0f) * 10.0f;
    case 2: return ntc_celsius(ohms, 10000.0f) * 10.0f;
    case 3: return gm_coolant_celsius(ohms) * 10.0f;
    default: return 0.0f;
  }
}

static float interpolate(const ChannelCal &k, float raw) {
  int i = 0;
  while (i < (int)k.n - 2 && raw > (float)k.pts[i + 1].raw) i++;
  const float r0 = k.pts[i].raw, r1 = k.pts[i + 1].raw;
  const float v0 = k.pts[i].value, v1 = k.pts[i + 1].value;
  return v0 + (v1 - v0) * (raw - r0) / (r1 - r0);
}

/** A reading at the short end of the scale (a sender at 0 ohms) is a short circuit only if the calibration cannot
 *  legitimately read there: a 0-30 ohm fuel sender reads 0 counts when empty. */
static bool short_is_a_fault(const Calibration &c, uint8_t ch, const ChannelCal &k) {
  float low_raw;  // the counts a healthy sender gives at the short end of its range
  if (k.n >= 1) {
    low_raw = k.topology == PULL_UP ? 1023.0f : 0.0f;
    for (uint8_t i = 0; i < k.n; i++) {
      const float r = k.pts[i].raw;
      low_raw = k.topology == PULL_UP ? (r < low_raw ? r : low_raw) : (r > low_raw ? r : low_raw);
    }
  } else if (ch == FUEL && k.preset != 0) {
    const FuelPreset &p = kFuel[k.preset];
    low_raw = ohms_to_raw(k, p.empty < p.full ? p.empty : p.full);
  } else {
    low_raw = ohms_to_raw(k, 50.0f);  // a temperature sender under 50 ohms is a short in practice
  }
  (void)c;
  return k.topology == PULL_UP ? low_raw > 20.0f : low_raw < 1003.0f;
}

static int32_t clamp_value(const Calibration &c, uint8_t ch, float v) {
  float lo = 0.0f, hi = 0.0f;
  switch (ch) {
    case FUEL: lo = 0.0f; hi = (float)c.tank_x10; break;
    case TEMP: lo = -400.0f; hi = 2000.0f; break;
    default: lo = 0.0f; hi = 2500.0f; break;
  }
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return (int32_t)lroundf(v);
}

Reading convert(const Calibration &c, uint8_t ch, float raw) {
  const ChannelCal &k = channel(c, ch);
  Reading r;
  const bool pu = k.topology == PULL_UP;
  if (pu ? raw >= 1015.0f : raw <= 8.0f) {
    r.status = ST_OPEN;
    return r;
  }
  if ((pu ? raw <= 2.0f : raw >= 1021.0f) && short_is_a_fault(c, ch, k)) {
    r.status = ST_SHORT;
    return r;
  }
  if (!calibrated(c, ch)) {
    r.status = ST_NOT_READY;
    return r;
  }
  float v;
  if (k.n >= 2) {
    v = interpolate(k, raw);
  } else {
    v = preset_value(c, ch, k, raw_to_ohms(k, raw));
    if (k.n == 1) v += (float)k.pts[0].value - preset_value(c, ch, k, raw_to_ohms(k, (float)k.pts[0].raw));
  }
  if (ch == TEMP) v += (float)k.offset;
  r.value = clamp_value(c, ch, v);
  r.status = ST_OK;
  return r;
}

Reading convert_ambient(const Calibration &c, float raw) {
  Reading r;
  if (raw >= 1015.0f) {
    r.status = ST_OPEN;
    return r;
  }
  if (raw <= 2.0f) {
    r.status = ST_SHORT;
    return r;
  }
  const float bias = c.amb_ntc ? 10000.0f : 100000.0f;  // the bridged value is the NTC's own resistance at 25 C
  const float ohms = bias * raw / (1023.0f - raw);
  float t = ntc_celsius(ohms, bias) * 10.0f + (float)c.amb_offset;
  if (t < -600.0f) t = -600.0f;
  if (t > 1000.0f) t = 1000.0f;
  r.value = (int32_t)lroundf(t);
  r.status = ST_OK;
  return r;
}

float battery_volts(const Calibration &c, float raw) {
  return raw * (5.0f / 1023.0f) * (122.0f / 22.0f) * (1.0f + (float)c.batt_trim_ppm / 1.0e6f);
}

int32_t battery_trim_for(float raw, float meter_volts) {
  const float plain = raw * (5.0f / 1023.0f) * (122.0f / 22.0f);
  if (plain < 0.5f) return 0;
  float ppm = (meter_volts / plain - 1.0f) * 1.0e6f;
  if (ppm > 50000.0f) ppm = 50000.0f;
  if (ppm < -50000.0f) ppm = -50000.0f;
  return (int32_t)lroundf(ppm);
}

// ---- points

bool monotonic(const ChannelCal &k) {
  if (k.n < 2) return true;
  int dir = 0;
  for (uint8_t i = 0; i + 1 < k.n; i++) {
    if (k.pts[i + 1].raw <= k.pts[i].raw) return false;
    const int d = k.pts[i + 1].value > k.pts[i].value ? 1 : k.pts[i + 1].value < k.pts[i].value ? -1 : 0;
    if (d == 0) return false;
    if (dir == 0) dir = d;
    if (d != dir) return false;
  }
  return true;
}

void clear_points(ChannelCal &k) {
  k.n = 0;
  for (Point &p : k.pts) p = Point();
}

bool capture(ChannelCal &k, uint8_t ch, uint16_t raw, int16_t value) {
  ChannelCal t = k;
  const uint8_t cap = max_points(ch);
  int hit = -1;
  for (uint8_t i = 0; i < t.n; i++) {
    const int d = (int)t.pts[i].raw - (int)raw;
    if (d >= -3 && d <= 3) hit = i;
  }
  if (hit < 0 && t.n >= cap) {  // full: replace the nearest
    int best = 0;
    int32_t bd = 100000L;
    for (uint8_t i = 0; i < t.n; i++) {
      int32_t d = (int32_t)t.pts[i].raw - (int32_t)raw;
      if (d < 0) d = -d;
      if (d < bd) {
        bd = d;
        best = i;
      }
    }
    hit = best;
  }
  if (hit >= 0) {  // drop it, then insert the new one in order
    for (uint8_t i = (uint8_t)hit; i + 1 < t.n; i++) t.pts[i] = t.pts[i + 1];
    t.n--;
  }
  uint8_t at = 0;
  while (at < t.n && t.pts[at].raw < raw) at++;
  for (uint8_t i = t.n; i > at; i--) t.pts[i] = t.pts[i - 1];
  t.pts[at].raw = raw;
  t.pts[at].value = value;
  t.n++;
  if (!monotonic(t)) return false;
  k = t;
  return true;
}

// ---- storage

namespace {

struct Out {
  uint8_t *p;
  size_t cap, n = 0;
  bool over = false;
  void u8(uint8_t v) {
    if (n < cap) p[n++] = v;
    else over = true;
  }
  void u16(uint16_t v) {
    u8((uint8_t)v);
    u8((uint8_t)(v >> 8));
  }
  void u32(uint32_t v) {
    u16((uint16_t)v);
    u16((uint16_t)(v >> 16));
  }
};

struct In {
  const uint8_t *p;
  size_t n, i = 0;
  size_t left() const { return n - i; }
  uint8_t u8() { return i < n ? p[i++] : 0; }
  uint16_t u16() {
    const uint16_t a = u8();
    return (uint16_t)(a | (uint16_t)u8() << 8);
  }
  uint32_t u32() {
    const uint32_t a = u16();
    return a | (uint32_t)u16() << 16;
  }
};

void put_channel(Out &o, uint8_t ch, const ChannelCal &k) {
  o.u8(k.topology);
  o.u32(k.bias_x10);
  o.u8(k.preset);
  o.u16((uint16_t)k.offset);
  const uint8_t n = k.n > max_points(ch) ? max_points(ch) : k.n;
  o.u8(n);
  for (uint8_t i = 0; i < n; i++) {
    o.u16(k.pts[i].raw);
    o.u16((uint16_t)k.pts[i].value);
  }
}

/** False when the bytes ran out before the channel's fixed part ended (the defaults stay). */
bool get_channel(In &in, uint8_t ch, ChannelCal &k) {
  if (in.left() < 9) return false;
  ChannelCal t;
  t.topology = in.u8();
  t.bias_x10 = in.u32();
  t.preset = in.u8();
  t.offset = (int16_t)in.u16();
  uint8_t n = in.u8();
  if (n > max_points(ch)) n = max_points(ch);
  if (in.left() < (size_t)n * 4) return false;
  for (uint8_t i = 0; i < n; i++) {
    t.pts[i].raw = in.u16();
    t.pts[i].value = (int16_t)in.u16();
  }
  t.n = n;
  if (t.topology > PULL_DOWN || t.bias_x10 == 0) return true;  // nonsense: keep the defaults, carry on
  if (ch == FUEL ? t.preset >= kFuelPresets : t.preset >= kTempPresets) t.preset = 0;
  if (ch == OIL) t.preset = 0;
  if (!monotonic(t)) t.n = 0;
  k = t;
  return true;
}

}  // namespace

size_t serialize(const Calibration &c, uint8_t *out, size_t cap) {
  Out o{out, cap};
  o.u16(c.flags);
  put_channel(o, FUEL, c.fuel);
  put_channel(o, TEMP, c.temp);
  put_channel(o, OIL, c.oil);
  o.u8(c.amb_ntc);
  o.u16((uint16_t)c.amb_offset);
  o.u16(c.tank_x10);
  o.u32((uint32_t)c.batt_trim_ppm);
  o.u32(c.speed_ppm_x100);
  o.u8(c.tach_ppr);
  return o.over ? 0 : o.n;
}

bool deserialize(const uint8_t *p, size_t n, Calibration &c) {
  if (n < 2) return false;
  Calibration t;
  defaults(t);
  In in{p, n};
  t.flags = in.u16();
  if (get_channel(in, FUEL, t.fuel) && get_channel(in, TEMP, t.temp) && get_channel(in, OIL, t.oil)) {
    if (in.left() >= 3) {
      const uint8_t ntc = in.u8();
      const int16_t off = (int16_t)in.u16();
      if (ntc <= 1) t.amb_ntc = ntc;
      t.amb_offset = off;
    }
    if (in.left() >= 2) {
      const uint16_t tank = in.u16();
      if (tank >= 50 && tank <= 4000) t.tank_x10 = tank;
    }
    if (in.left() >= 4) {
      const int32_t trim = (int32_t)in.u32();
      if (trim >= -50000 && trim <= 50000) t.batt_trim_ppm = trim;
    }
    if (in.left() >= 4) {
      const uint32_t ppm = in.u32();
      if (ppm >= 50000 && ppm <= 5000000) t.speed_ppm_x100 = ppm;
    }
    if (in.left() >= 1) {
      const uint8_t ppr = in.u8();
      if (ppr <= 16) t.tach_ppr = ppr;
    }
  }
  c = t;
  return true;
}

size_t block_encode(const Calibration &c, uint32_t seq, uint8_t *out, size_t cap) {
  if (cap < kBlockMax) return 0;
  uint8_t payload[kPayloadMax];
  const size_t pn = serialize(c, payload, sizeof(payload));
  if (!pn) return 0;
  size_t n = 0;
  out[n++] = 0xCA;
  out[n++] = 0x1B;
  out[n++] = 1;  // format version
  out[n++] = 0;
  for (int i = 0; i < 4; i++) out[n++] = (uint8_t)(seq >> (8 * i));
  out[n++] = (uint8_t)pn;
  out[n++] = (uint8_t)(pn >> 8);
  memcpy(out + n, payload, pn);
  n += pn;
  const uint16_t crc = elc_ch_crc16(out, n);
  out[n++] = (uint8_t)crc;
  out[n++] = (uint8_t)(crc >> 8);
  return n;
}

bool block_decode(const uint8_t *p, size_t n, uint32_t *seq, Calibration &c) {
  if (n < 12 || p[0] != 0xCA || p[1] != 0x1B || p[2] < 1) return false;
  const size_t pn = (size_t)(p[8] | (p[9] << 8));
  if (pn > kPayloadMax || 10 + pn + 2 > n) return false;
  const uint16_t crc = (uint16_t)(p[10 + pn] | (p[10 + pn + 1] << 8));
  if (elc_ch_crc16(p, 10 + pn) != crc) return false;
  if (!deserialize(p + 10, pn, c)) return false;
  if (seq) *seq = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
  return true;
}

}  // namespace cal
