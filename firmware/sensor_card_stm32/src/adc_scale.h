#pragma once
// From the STM32's 12-bit, 3.3 V ADC to the counts rev A's 10-bit, 5 V ADC gave, which the calibration, the screens'
// formulas and the stored points are all written in. No hardware: host-tested.
#include <stdint.h>

static const float kAdcVref = 3.3f;
static const float kAdcMax12 = 4095.0f;

/** A ratiometric sender channel: the bank resistor runs from the same 3.3 V the ADC measures against, so a sender
 *  resistance gives the same fraction of full scale it gave on rev A; only the full scale changes, 4095 -> 1023. */
static inline float adc_sender_counts(uint16_t c12) { return (float)c12 * (1023.0f / kAdcMax12); }

/** The battery divider is the same 100k / 22k but the reference is 3.3 V, not 5 V: the counts are rescaled to what a
 *  5 V reference would have read, so cal::battery_volts and the stored trim stay as they are. Full scale is now
 *  3.3 x 122 / 22 = 18.3 V (675 rev A counts). */
static inline float adc_batt_counts(uint16_t c12) { return (float)c12 * (1023.0f / kAdcMax12) * (kAdcVref / 5.0f); }

/** Volts at the pin, and at the slot behind a divider. */
static inline float adc_pin_volts(uint16_t c12) { return (float)c12 * (kAdcVref / kAdcMax12); }
static inline float adc_slot_volts(uint16_t c12, float top_k, float bottom_k) {
  return adc_pin_volts(c12) * (top_k + bottom_k) / bottom_k;
}

/** A level input read through the ADC with hysteresis (ignition, alternator L). */
struct LevelInput {
  float top_k, bottom_k, on_v, off_v;
  bool state = false;
  LevelInput(float top, float bottom, float on, float off) : top_k(top), bottom_k(bottom), on_v(on), off_v(off) {}
  bool update(uint16_t c12) {
    const float v = adc_slot_volts(c12, top_k, bottom_k);
    if (v >= on_v) state = true;
    else if (v <= off_v) state = false;
    return state;
  }
};
