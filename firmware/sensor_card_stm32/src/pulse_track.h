#pragma once
// A pulse channel timed by input capture: 16-bit capture timestamps extended to 32 bits by counting overflows (rev A's
// scheme, kept as is), at 0.5 us a tick. Pulses closer than the hold-off are ignored (ringing, points bounce).
// The capture and overflow calls run in interrupts; snapshot() with them masked. No hardware: host-tested.
#include <stdint.h>

struct PulseInfo {
  uint32_t period_us = 0;  // between the last two accepted pulses; 0 = none yet, or none for 3 s
  uint32_t since_us = 0;   // since the last accepted pulse
  uint32_t count = 0;      // pulses accepted since boot (wraps)
};

struct PulseTrack {
  volatile uint16_t ovf = 0;
  volatile uint32_t last = 0;    // extended timestamp of the last accepted pulse, ticks
  volatile uint32_t period = 0;  // ticks
  volatile uint32_t count = 0;
  volatile uint8_t idle = 0;     // overflows since the last pulse
  volatile bool seen = false;
  const uint32_t min_ticks;      // hold-off

  explicit PulseTrack(uint32_t hold_off_ticks) : min_ticks(hold_off_ticks) {}

  /** A capture: `ccr` is the timer's capture register, `ovf_pending` whether the overflow flag is set and its
   *  interrupt has not run yet (then a capture in the lower half of the range came after that overflow). */
  void captured(uint16_t ccr, bool ovf_pending) {
    uint32_t ext = ((uint32_t)ovf << 16) | ccr;
    if (ovf_pending && ccr < 0x8000) ext += 0x10000UL;
    if (!seen) {
      seen = true;
      last = ext;
      count = count + 1;
      idle = 0;
      return;
    }
    const uint32_t elapsed = ext - last;
    if (elapsed >= min_ticks) {
      period = elapsed;
      last = ext;
      count = count + 1;
      idle = 0;
    }
  }

  void overflowed() {
    ovf = ovf + 1;
    if (idle < 255 && ++idle > 100) period = 0;  // ~3.3 s with no pulse: stopped
  }

  /** `cnt` is the timer's counter and `ovf_pending` its overflow flag, read with interrupts masked. */
  PulseInfo snapshot(uint16_t cnt, bool ovf_pending) const {
    PulseInfo p;
    p.count = count;
    if (!seen || !period) return p;
    uint32_t now = ((uint32_t)ovf << 16) | cnt;
    if (ovf_pending && cnt < 0x8000) now += 0x10000UL;
    p.period_us = period / 2;
    p.since_us = (now - last) / 2;
    return p;
  }
};

static const uint32_t kSpeedMinTicks = 1000;  // 500 us: a hall sensor cannot be faster
// 1 ms hold-off after an accepted tach pulse: breaker points bounce for 0.1-1 ms. A V8 at 8000 rpm fires 533 times a
// second (1.9 ms apart), so 1 ms passes up to 1000 pulses a second (15000 rpm on a V8, 20000 on a six).
static const uint32_t kTachMinTicks = 2000;   // 1 ms (0.5 us ticks)
