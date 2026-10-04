#include "engine.h"

#include <math.h>

static const float kLitresPerGallon = 3.785411784f;
static const float kWheelMetres = 2.1f;  // the screens' wheel (WHEEL_CIRCUMFERENCE_M): legacy "wheel rpm" is in it
static const uint8_t kFaultCount = 10;   // 500 ms at 20 Hz
static const uint16_t kCoilMissingSteps = 100;  // 5 s
static const uint16_t kAltStuckSteps = 600;     // 30 s
static const float kMpgMax = 200.0f;

uint16_t Engine::legacyCoolantCounts(float celsius) {
  const float t = celsius + 273.15f;
  const float x = expf(3950.0f * (1.0f / t - 1.0f / 298.15f));  // thermistor over the screens' 100k series resistor
  float raw = 1023.0f / (1.0f + x);
  if (raw < 1.0f) raw = 1.0f;
  if (raw > 1022.0f) raw = 1022.0f;
  return (uint16_t)lroundf(raw);
}

void Engine::sender(Debounce &d, const cal::Reading &r, bool feed_ok, bool has_feed) {
  if (has_feed) {
    if (!feed_ok) {
      if (d.power < 255) d.power++;
    } else {
      d.power = 0;
    }
    d.power_bad = d.power >= kFaultCount;
  }
  if (d.power_bad) {  // a tripped feed explains the reading: do not also call it open
    d.low = d.high = 0;
    d.open = d.shorted = false;
    return;
  }
  if (r.status == cal::ST_OPEN) { if (d.low < 255) d.low++; } else d.low = 0;
  if (r.status == cal::ST_SHORT) { if (d.high < 255) d.high++; } else d.high = 0;
  d.open = d.low >= kFaultCount;
  d.shorted = d.high >= kFaultCount;
}

static uint8_t effective(bool power_bad, bool open, bool shorted, uint8_t reading, uint8_t *prev) {
  uint8_t st;
  if (power_bad) st = cal::ST_FAULT;
  else if (open) st = cal::ST_OPEN;
  else if (shorted) st = cal::ST_SHORT;
  else if (reading == cal::ST_OPEN || reading == cal::ST_SHORT) st = *prev;  // not yet debounced
  else st = reading;
  *prev = st;
  return st;
}

static uint32_t period_us(uint32_t period, uint32_t since) {
  if (!period || since > 2000000UL) return 0;
  return since > period ? since : period;  // while slowing, the next pulse is at least `since` away
}

void Engine::step(const EngineIn &in, EngineOut &out) {
  const cal::Calibration &c = *_cal;
  for (int i = 0; i < 4; i++) _raw[i] = in.raw[i];
  _batt_raw = in.batt_raw;

  // ---- senders
  const cal::Reading rf = cal::convert(c, cal::FUEL, in.raw[0]);
  const cal::Reading rt = cal::convert(c, cal::TEMP, in.raw[1]);
  const cal::Reading ro = cal::convert(c, cal::OIL, in.raw[2]);
  const cal::Reading ra = cal::convert_ambient(c, in.raw[3]);
  sender(_d_fuel, rf, true, false);
  sender(_d_temp, rt, in.temp_feed_ok, true);
  sender(_d_oil, ro, in.press_feed_ok, true);
  sender(_d_amb, ra, true, false);
  out.st_fuel = effective(_d_fuel.power_bad, _d_fuel.open, _d_fuel.shorted, rf.status, &_prev_st[0]);
  out.st_temp = effective(_d_temp.power_bad, _d_temp.open, _d_temp.shorted, rt.status, &_prev_st[1]);
  out.st_oil = effective(_d_oil.power_bad, _d_oil.open, _d_oil.shorted, ro.status, &_prev_st[2]);
  out.st_amb = effective(_d_amb.power_bad, _d_amb.open, _d_amb.shorted, ra.status, &_prev_st[3]);
  out.temp_c10 = rt.status == cal::ST_OK ? rt.value : 0;
  out.oil_kpa = ro.status == cal::ST_OK ? ro.value : 0;
  out.amb_c10 = ra.status == cal::ST_OK ? ra.value : 0;
  out.batt_v100 = (int32_t)lroundf(cal::battery_volts(c, in.batt_raw) * 100.0f);

  // ---- speed, engine speed, distance
  const uint32_t sp = period_us(in.speed_period_us, in.speed_since_us);
  _mph = sp ? 3.6e11f / ((float)sp * (float)c.speed_ppm_x100) : 0.0f;
  out.mph = _mph;
  out.wheel_rpm = (uint32_t)lroundf(_mph * 1609.344f / 60.0f / kWheelMetres);
  const uint32_t tp = period_us(in.tach_period_us, in.tach_since_us);
  out.engine_rpm = (tp && in.tach_ppr) ? (60000000UL / in.tach_ppr) / tp : 0;

  if (!_pulses_init) {
    _pulses_last = in.speed_pulses;
    _pulses_init = true;
  }
  const uint32_t dp = in.speed_pulses - _pulses_last;
  _pulses_last = in.speed_pulses;
  _frac_ms += in.dt_ms;
  if (dp > 0 && dp < 5000) {
    const uint64_t nano = (uint64_t)dp * 100000000000ULL / c.speed_ppm_x100;  // 1 mile = 1e9 nano-miles
    _odo->add((uint32_t)nano);
  }
  if (_frac_ms >= 2000) {  // sub-tenth progress, so a power cut does not lose it
    _frac_ms = 0;
    if (_mph > 0.05f && _odo->known() && _odo->fracNano() > 0) _odo->stampFrac();
  }

  // ---- fuel level: slosh-damped over about 20 s, a refuel followed at once
  float vol = 0;
  const bool fuel_ok = rf.status == cal::ST_OK;
  if (fuel_ok) {
    vol = (float)rf.value;
    if (!_fuel_valid) {
      _fuel_damped = _fuel_fast = vol;
      _fuel_valid = true;
    } else {
      _fuel_fast += (vol - _fuel_fast) / 32.0f;  // the old firmware's EMA, for the instant economy
      float a = (float)in.dt_ms / 20000.0f;
      if (a > 1.0f) a = 1.0f;
      _fuel_damped += (vol - _fuel_damped) * a;
      const bool stopped = out.engine_rpm < 400 || _mph < 1.0f;
      if (vol - _fuel_damped > 0.03f * (float)c.tank_x10 && stopped) {
        _refuel_ms += in.dt_ms;
        if (_refuel_ms >= 10000) {
          _fuel_damped = _fuel_fast = vol;
          _refuel_ms = 0;
        }
      } else {
        _refuel_ms = 0;
      }
    }
  }
  const bool fuel_shown = fuel_ok && _fuel_valid;
  out.st_fuel = out.st_fuel == cal::ST_OK && !_fuel_valid ? (uint8_t)cal::ST_NOT_READY : out.st_fuel;
  out.fuel_l10 = fuel_shown ? (int32_t)lroundf(_fuel_damped) : 0;
  out.fuel_pct10 = (fuel_shown && c.tank_x10) ? (int32_t)lroundf(_fuel_damped * 1000.0f / (float)c.tank_x10) : 0;
  if (out.fuel_pct10 > 1000) out.fuel_pct10 = 1000;

  // ---- ignition, alternator, faults
  out.ign = in.ign;
  out.charging = in.ign && in.alt_high;
  out.flags = (uint8_t)((in.alt_high ? kFlagAltOk : 0) | (in.ign ? kFlagIgnOn : 0));
  if (in.alt_high) _alt_hi = true; else _alt_lo = true;
  if (in.ign && out.engine_rpm == 0) { if (_coil_missing < 0xFFFF) _coil_missing++; } else _coil_missing = 0;
  if (in.ign && out.engine_rpm > 500) {
    if ((!_alt_hi || !_alt_lo) && _alt_stuck < 0xFFFF) _alt_stuck++;
  }
  uint16_t f = 0;
  if (_d_fuel.open) f |= FAULT_POT_OPEN;
  if (_d_fuel.shorted) f |= FAULT_POT_SHORT;
  if (_d_temp.open) f |= FAULT_TEMP_OPEN;
  if (_d_temp.shorted) f |= FAULT_TEMP_SHORT;
  if (_d_oil.open) f |= FAULT_PRESS_OPEN;
  if (_d_oil.shorted) f |= FAULT_PRESS_SHORT;
  if (_d_temp.power_bad) f |= FAULT_TEMP_POWER;
  if (_d_oil.power_bad) f |= FAULT_PRESS_POWER;
  if (_coil_missing >= kCoilMissingSteps) f |= FAULT_COIL_MISSING;
  const bool alt_stuck = _alt_stuck >= kAltStuckSteps && (!_alt_hi || !_alt_lo);
  if (alt_stuck) f |= FAULT_ALT_STUCK;
  if (!_odo->known() || !_odo->persisted()) f |= FAULT_ODO_STORE;
  out.faults = f;
  out.st_rpm = (_coil_missing >= kCoilMissingSteps) ? cal::ST_NOT_READY : cal::ST_OK;
  out.st_charge = alt_stuck ? cal::ST_FAULT : cal::ST_OK;
  out.st_odo = !_odo->known() ? cal::ST_NOT_READY : (f & FAULT_ODO_STORE) ? cal::ST_FAULT : cal::ST_OK;
  out.odo_tenths = _odo->known() ? _odo->tenths() : kOdoUnknown;

  const float tank_gal = (float)c.tank_x10 / 10.0f / kLitresPerGallon;
  (void)tank_gal;
  economy(_fuel_damped / 10.0f / kLitresPerGallon, _fuel_fast / 10.0f / kLitresPerGallon, fuel_shown, in.ign, _mph,
          out);
  legacy(out);
}

void Engine::economy(float gal, float fast_gal, bool fuel_ok, bool ign, float mph, EngineOut &out) {
  out.mpg_inst_x10 = out.mpg_avg_x10 = 0;
  out.dte_tenths = 0;
  out.has_econ = fuel_ok;
  if (!fuel_ok) return;
  const float tank_gal = (float)_cal->tank_x10 / 10.0f / kLitresPerGallon;

  _ring[_ring_i] = fast_gal;
  _ring_i = (uint8_t)((_ring_i + 1) % kRing);
  if (_ring_n < kRing) _ring_n++;

  // key-on, or the stored odometer just loaded late: a fresh trip baseline
  const bool odo_just_loaded = _odo->known() && !_prev_odo_known;
  _prev_odo_known = _odo->known();
  if ((ign && !_prev_ign) || odo_just_loaded) {
    _trip_t0 = _odo->tenths();
    _trip_f0 = _odo->fracNano();
    _trip_gal0 = gal;
  }
  _prev_ign = ign;

  float inst = 0;
  if (_ring_n >= kRing && ign) {
    const float delta = _ring[_ring_i] - fast_gal;  // the oldest sample is the one about to be overwritten
    const float window_s = (float)kRing * (float)kStepMs / 1000.0f;
    if (delta > 0.0f) {
      const float gph = delta / (window_s / 3600.0f);
      if (gph >= 0.02f && mph >= 3.0f) inst = mph / gph;
    }
  }
  if (inst > kMpgMax) inst = kMpgMax;
  if (inst > 0.5f) out.mpg_inst_x10 = (uint16_t)(inst * 10.0f + 0.5f);

  if (ign && gal > _trip_gal0 + 0.03f * tank_gal) {  // refuelled
    _trip_t0 = _odo->tenths();
    _trip_f0 = _odo->fracNano();
    _trip_gal0 = gal;
  }
  float avg = 0;
  if (ign) {
    float miles = 0;
    if (_odo->tenths() >= _trip_t0) {
      miles = (float)(_odo->tenths() - _trip_t0) * 0.1f + (float)((int32_t)_odo->fracNano() - (int32_t)_trip_f0) / 1.0e9f;
    }
    if (miles < 0) miles = 0;
    float used = _trip_gal0 - gal;
    if (used < 0) used = 0;
    if (miles > 0.05f && used > 0.01f) avg = miles / used;
  }
  if (avg > kMpgMax) avg = kMpgMax;
  if (avg > 0.5f) out.mpg_avg_x10 = (uint16_t)(avg * 10.0f + 0.5f);

  const float mpg = avg > 1.0f ? avg : inst > 1.0f ? inst : 0.0f;
  if (gal > 0 && mpg > 0) {
    uint32_t t = (uint32_t)(gal * mpg * 10.0f + 0.5f);
    out.dte_tenths = t > 99999990UL ? 99999990UL : t;
  }
}

void Engine::legacy(EngineOut &out) const {
  float pct = (float)out.fuel_pct10 / 10.0f;
  out.pot_adc = out.st_fuel == cal::ST_OK ? (uint16_t)lroundf(pct * 10.23f) : 0;
  out.therm_adc = out.st_temp == cal::ST_OK ? legacyCoolantCounts((float)out.temp_c10 / 10.0f) : 0;
  float psi = (float)out.oil_kpa / 6.894757f;
  if (psi > 100.0f) psi = 100.0f;
  out.pressure_adc = out.st_oil == cal::ST_OK ? (uint16_t)lroundf(psi * 10.23f) : 0;
  out.ambient_adc = out.st_amb == cal::ST_OK ? legacyCoolantCounts((float)out.amb_c10 / 10.0f) : 0;
  out.charge_adc = !out.ign ? 400 : out.charging ? 720 : 530;  // the amp gauge's bands
}

static uint16_t triangle(uint32_t tick) {  // 0..1023 over 1200 ticks (60 s at 20 Hz)
  const uint32_t half = 600;
  uint32_t m = tick % (2 * half);
  if (m < half) return (uint16_t)(m * 1023UL / half);
  m -= half;
  return (uint16_t)((half - m) * 1023UL / half);
}

void Engine::demo(EngineOut &out) {
  const uint32_t t = ++_demo_tick;
  const uint16_t tf = triangle(t), te = triangle(t + 80), tt = triangle(t + 160), ta = triangle(t + 240),
                 tp = triangle(t + 320), tv = triangle(t + 400), ts = triangle(t + 480);
  out = EngineOut();
  out.ign = out.charging = true;
  out.flags = kFlagAltOk | kFlagIgnOn;
  out.fuel_pct10 = (int32_t)(tf * 1000UL / 1023);
  out.fuel_l10 = (int32_t)((uint32_t)out.fuel_pct10 * _cal->tank_x10 / 1000);
  out.temp_c10 = (int32_t)(400 + tt * 700UL / 1023);
  out.oil_kpa = (int32_t)(tp * 600UL / 1023);
  out.amb_c10 = (int32_t)(-100 + ta * 500L / 1023);
  out.batt_v100 = (int32_t)(1150 + tv * 300UL / 1023);
  out.wheel_rpm = ts * 2400UL / 1023;
  out.mph = (float)out.wheel_rpm * 1.0f / (1609.344f / 60.0f / kWheelMetres);
  out.engine_rpm = te * 6500UL / 1023;
  out.st_fuel = out.st_temp = out.st_oil = out.st_amb = cal::ST_OK;
  out.odo_tenths = _odo->known() ? _odo->tenths() : kOdoUnknown;
  out.st_odo = _odo->known() ? cal::ST_OK : cal::ST_NOT_READY;
  out.mpg_inst_x10 = (uint16_t)(100 + triangle(t + 560) * 180UL / 1023);
  out.mpg_avg_x10 = (uint16_t)(120 + triangle(t + 200) * 100UL / 1023);
  out.has_econ = true;
  const float gal = (float)out.fuel_l10 / 10.0f / kLitresPerGallon;
  out.dte_tenths = (uint32_t)(gal * ((float)out.mpg_avg_x10 / 10.0f) * 10.0f + 0.5f);
  legacy(out);
}
