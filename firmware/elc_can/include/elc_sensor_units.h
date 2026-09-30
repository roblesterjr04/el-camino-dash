#pragma once

// Sensor pack ADC readings in the units the gauges show. Shared by the displays (sensor_packet.cpp, from the
// UART) and the console (from CAN 0x100-0x104), so both agree. Header-only, no Arduino dependency.

#include <math.h>
#include <stdint.h>

static inline float elc_units_map_adc(uint16_t raw, float out_lo, float out_hi) {
  float t = (float)raw / 1023.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return out_lo + (out_hi - out_lo) * t;
}

static inline float elc_units_fuel_pct(uint16_t adc) { return elc_units_map_adc(adc, 0.0f, 100.0f); }
static inline float elc_units_oil_psi(uint16_t adc) { return elc_units_map_adc(adc, 0.0f, 100.0f); }
/** Charge meter, -100..100 (the same linear mapping as 10-16 V on the old Pi gauge). */
static inline float elc_units_amp(uint16_t adc) { return elc_units_map_adc(adc, -100.0f, 100.0f); }

/** Coolant, 100k NTC (B3950) against 100k; 100 °F when the reading is out of range. */
static inline float elc_units_coolant_f(uint16_t adc) {
  if (adc == 0 || adc >= 1023) return 100.0f;
  const float r = 100000.0f * ((1023.0f / (float)adc) - 1.0f);
  if (r <= 0.0f) return 100.0f;
  float s = logf(r / 100000.0f) / 3950.0f;
  s += 1.0f / (25.0f + 273.15f);
  const float c = (1.0f / s) - 273.15f;
  if (!isfinite(c)) return 100.0f;
  return c * 9.0f / 5.0f + 32.0f;
}

// Sensor pack flags (sensor_uart.md): bit0 alternator charging, bit1 ignition on.
static const uint8_t ELC_SENSOR_FLAG_ALT_OK = 0x01;
static const uint8_t ELC_SENSOR_FLAG_IGN_ON = 0x02;
