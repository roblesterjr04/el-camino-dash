#pragma once
#include "pulse_track.h"

// Tach (TIM1_CH1, PA8) and speed (TIM3_CH1, PB4) by input capture on the falling edge, 2 MHz ticks (0.5 us), the
// 16-bit counters extended by the overflow interrupt (pulse_track.h).
void capture_begin();
PulseInfo capture_speed();
PulseInfo capture_tach();
