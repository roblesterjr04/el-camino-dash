#pragma once
#include <stdint.h>

// Free-running ADC scan in the background: fuel, engine temp, oil, ambient, battery, WAKE sense (ADC0-3, 8, 9), each
// visited about every 0.7 ms. take() hands back the average since the last take, so the 50 ms step reads about 70
// samples per channel and nothing ever blocks on a conversion.
enum AdcSlot : uint8_t { SLOT_FUEL, SLOT_TEMP, SLOT_OIL, SLOT_AMB, SLOT_BATT, SLOT_WAKE, SLOT_COUNT };

void adc_begin();
/** Average counts (0..1023) since the previous take of this slot; false if there were no samples. */
bool adc_take(AdcSlot s, float *avg);
