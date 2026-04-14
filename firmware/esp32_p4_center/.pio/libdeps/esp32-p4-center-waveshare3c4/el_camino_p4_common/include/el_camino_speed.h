#pragma once

#include <math.h>
#include <stdint.h>

// Same math as src/dashboard/input/arduino_serial.py (wheel_circumference_m, speed_pulses_per_rev).
static inline float elc_speed_rpm_to_mph(uint32_t speed_rpm, float wheel_circumference_m,
                                         float speed_pulses_per_rev) {
  if (speed_pulses_per_rev <= 0.0f || wheel_circumference_m <= 0.0f) {
    return 0.0f;
  }
  const float wheel_rps = ((float)speed_rpm / speed_pulses_per_rev) / 60.0f;
  const float speed_mps = wheel_rps * wheel_circumference_m;
  return speed_mps * 2.236936f;
}
