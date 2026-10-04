#pragma once
#include <stdint.h>

// Tach (ICP4, Timer4) and speed (ICP5, Timer5) by input capture: a timestamp per falling edge at 2 MHz (0.5 us),
// extended past 16 bits by the overflow interrupt. Pulses closer than the debounce time are ignored (ringing).
struct PulseInfo {
  uint32_t period_us = 0;  // between the last two accepted pulses; 0 = none yet, or none for 3 s
  uint32_t since_us = 0;   // since the last accepted pulse
  uint32_t count = 0;      // pulses accepted since boot (wraps)
};

void capture_begin();
PulseInfo capture_speed();
PulseInfo capture_tach();
