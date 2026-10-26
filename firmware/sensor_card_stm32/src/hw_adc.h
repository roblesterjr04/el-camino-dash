#pragma once
#include <stdint.h>

// ADC1 scanning the eight inputs (PA0 PA1 PA4 PA5 PA6 PA7 PB0 PB1 = IN0 1 4 5 6 7 8 9) by DMA into a ring of the last
// 32 scans, hardware-oversampled 16x, so nothing ever blocks on a conversion and no interrupt runs: one scan is about
// 1.4 ms, the ring about 44 ms, and take() averages what is in it. Counts come back on rev A's 10-bit scale
// (adc_scale.h), which is what the calibration and the screens expect.
enum AdcSlot : uint8_t { SLOT_FUEL, SLOT_TEMP, SLOT_OIL, SLOT_AMB, SLOT_ALT, SLOT_BATT, SLOT_WAKE, SLOT_IGN, SLOT_COUNT };

void adc_begin();
/** The slot's average over the ring, on rev A's scale (senders and WAKE: 0..1023 ratiometric; battery: rescaled to
 *  the 5 V reference; ALT / IGN: 0..1023 of 3.3 V). False until the first scan has landed. */
bool adc_take(AdcSlot s, float *avg);
/** The latest 12-bit sample of a slot (for `adc`). */
uint16_t adc_raw12(AdcSlot s);
/** The level inputs through their dividers with hysteresis: ignition on, alternator L high. */
bool adc_ign_on();
bool adc_alt_high();
