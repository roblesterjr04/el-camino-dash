#pragma once

// The card's readings, once per 50 ms: averaged counts and pulse timing in, calibrated values, the legacy frame fields,
// faults, fuel economy and the odometer out. No Arduino dependencies: host-tested.
//
// The legacy fields (frames 0x100-0x104 and the J8 serial frame) are what the three screens and the console read until
// they take the calibrated SENSOR values: the screens turn them into units with fixed formulas (elc_sensor_units.h, a
// straight line for fuel and oil, a 100k NTC for coolant). So the card sends counts that those formulas turn back into
// the calibrated value, and a channel that is not calibrated sends 0 (the formulas' "out of range": 100 F, 0 psi).

#include <stdint.h>

#include "calib.h"
#include "odometer.h"

static const uint32_t kStepMs = 50;  // 20 Hz

// sensor fault word (docs/protocols/sensor_uart.md)
enum : uint16_t {
  FAULT_POT_OPEN = 1u << 0, FAULT_POT_SHORT = 1u << 1, FAULT_TEMP_OPEN = 1u << 2, FAULT_TEMP_SHORT = 1u << 3,
  FAULT_PRESS_OPEN = 1u << 4, FAULT_PRESS_SHORT = 1u << 5, FAULT_COIL_MISSING = 1u << 9, FAULT_ALT_STUCK = 1u << 10,
  FAULT_TEMP_POWER = 1u << 12, FAULT_PRESS_POWER = 1u << 13, FAULT_ODO_STORE = 1u << 15,
};
static const uint8_t kFlagAltOk = 0x01, kFlagIgnOn = 0x02;

struct EngineIn {
  float raw[4] = {0, 0, 0, 0};  // averaged counts: fuel, engine temp, oil, ambient
  float batt_raw = 0;           // averaged counts of ADC8
  bool ign = false, alt_high = false, temp_feed_ok = true, press_feed_ok = true;
  // pulse timing (speed ICP5, tach ICP4), microseconds; period 0 = no pulse seen yet
  uint32_t speed_period_us = 0, speed_since_us = 0, speed_pulses = 0;
  uint32_t tach_period_us = 0, tach_since_us = 0;
  uint8_t tach_ppr = 8;  // after the J5 jumpers and the setting
  uint32_t dt_ms = kStepMs;
};

struct EngineOut {
  bool ign = false, charging = false;
  float mph = 0;
  uint32_t wheel_rpm = 0;   // legacy: with the screens' 2.1 m wheel it shows the true speed
  uint32_t engine_rpm = 0;
  // calibrated (units as in calib.h); status per sensor as on the SENSOR channel
  int32_t fuel_l10 = 0, fuel_pct10 = 0, temp_c10 = 0, oil_kpa = 0, amb_c10 = 0, batt_v100 = 0;
  uint8_t st_fuel = cal::ST_NOT_READY, st_temp = cal::ST_NOT_READY, st_oil = cal::ST_NOT_READY,
          st_amb = cal::ST_NOT_READY, st_rpm = cal::ST_OK, st_charge = cal::ST_OK, st_odo = cal::ST_NOT_READY,
          st_econ = cal::ST_NOT_READY;
  uint16_t faults = 0;
  uint8_t flags = 0;
  uint32_t odo_tenths = kOdoUnknown;
  uint16_t mpg_inst_x10 = 0, mpg_avg_x10 = 0;
  uint32_t dte_tenths = 0;
  bool has_econ = false;
  // legacy counts
  uint16_t pot_adc = 0, therm_adc = 0, pressure_adc = 0, charge_adc = 0, ambient_adc = 0;
};

class Engine {
 public:
  void begin(cal::Calibration *c, Odometer *odo) {
    _cal = c;
    _odo = odo;
  }
  /** One 50 ms step. */
  void step(const EngineIn &in, EngineOut &out);
  /** Bench: a slow sweep of everything instead of the senders (the J9 jumper). */
  void demo(EngineOut &out);

  /** The averaged raw counts of the last step, for captures and `cal`. */
  float lastRaw(uint8_t ch) const { return _raw[ch & 3]; }
  float lastBattRaw() const { return _batt_raw; }
  /** Fuel level the gauge shows (L x10), damped over about 20 s. */
  float fuelDamped() const { return _fuel_damped; }
  bool fuelValid() const { return _fuel_valid; }
  /** Speed pulses since the last call (for the GPS speed calibration), wraps like the counter. */
  uint32_t speedPulseTotal() const { return _pulses_last; }
  float mph() const { return _mph; }

  // legacy synthesis, exposed for the tests
  static uint16_t legacyCoolantCounts(float celsius);

 private:
  struct Debounce {
    uint8_t low = 0, high = 0, power = 0;
    bool open = false, shorted = false, power_bad = false;
  };
  void sender(Debounce &d, const cal::Reading &r, bool feed_ok, bool has_feed);
  void economy(float fuel_gal, float fast_gal, bool fuel_ok, bool ign, float mph, EngineOut &out);
  void legacy(EngineOut &out) const;
  cal::Calibration *_cal = nullptr;
  Odometer *_odo = nullptr;
  float _raw[4] = {0, 0, 0, 0};
  float _batt_raw = 0;
  float _mph = 0;
  // fuel
  bool _fuel_valid = false;
  float _fuel_damped = 0, _fuel_fast = 0;
  uint32_t _refuel_ms = 0;
  // faults
  Debounce _d_fuel, _d_temp, _d_oil, _d_amb;
  uint8_t _prev_st[4] = {cal::ST_NOT_READY, cal::ST_NOT_READY, cal::ST_NOT_READY, cal::ST_NOT_READY};
  uint16_t _coil_missing = 0, _alt_stuck = 0;
  bool _alt_hi = false, _alt_lo = false;
  // odometer pulses
  bool _pulses_init = false;
  uint32_t _pulses_last = 0;
  uint32_t _frac_ms = 0;
  // economy
  static const uint8_t kRing = 20;
  float _ring[kRing];
  uint8_t _ring_i = 0, _ring_n = 0;
  uint32_t _trip_t0 = 0, _trip_f0 = 0;
  float _trip_gal0 = 0;
  bool _prev_ign = false, _prev_odo_known = false;
  uint32_t _demo_tick = 0;
};
