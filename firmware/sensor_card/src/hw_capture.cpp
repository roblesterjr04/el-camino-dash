#include "hw_capture.h"

#include <Arduino.h>
#include <avr/interrupt.h>

#include "card_pins.h"

namespace {
struct Chan {
  volatile uint16_t ovf = 0;
  volatile uint32_t last = 0;     // extended timestamp of the last accepted pulse, ticks
  volatile uint32_t period = 0;   // ticks
  volatile uint32_t count = 0;
  volatile uint8_t idle = 0;      // overflows since the last pulse
  volatile bool seen = false;
};
Chan s_speed, s_tach;
const uint32_t kSpeedMinTicks = 1000;  // 500 us: a hall sensor cannot be faster
// 1 ms hold-off after an accepted tach pulse: breaker points bounce for 0.1-1 ms. A V8 at 8000 rpm fires 533 times a
// second (1.9 ms apart), so 1 ms passes up to 1000 pulses a second (15000 rpm on a V8, 20000 on a six).
const uint32_t kTachMinTicks = 2000;   // 1 ms (0.5 us ticks)

inline void captured(Chan &c, uint16_t icr, bool ovf_pending, uint32_t min_ticks) {
  uint32_t ext = ((uint32_t)c.ovf << 16) | icr;
  if (ovf_pending && icr < 0x8000) ext += 0x10000UL;  // the overflow came first and its interrupt has not run yet
  if (!c.seen) {
    c.seen = true;
    c.last = ext;
    c.count++;
    c.idle = 0;
    return;
  }
  const uint32_t elapsed = ext - c.last;
  if (elapsed >= min_ticks) {
    c.period = elapsed;
    c.last = ext;
    c.count++;
    c.idle = 0;
  }
}

inline void overflowed(Chan &c) {
  c.ovf++;
  if (c.idle < 255 && ++c.idle > 100) c.period = 0;  // ~3.3 s with no pulse: stopped
}

}  // namespace

ISR(TIMER4_CAPT_vect) { captured(s_tach, ICR4, TIFR4 & _BV(TOV4), kTachMinTicks); }
ISR(TIMER4_OVF_vect) { overflowed(s_tach); }
ISR(TIMER5_CAPT_vect) { captured(s_speed, ICR5, TIFR5 & _BV(TOV5), kSpeedMinTicks); }
ISR(TIMER5_OVF_vect) { overflowed(s_speed); }

void capture_begin() {
  pinMode(PIN_TACH, INPUT);   // no internal pull-ups: the card has its own (a 22k one on the tach opto)
  pinMode(PIN_SPEED, INPUT);
  noInterrupts();
  TCCR4A = 0;
  TCCR4B = _BV(ICNC4) | _BV(CS41);  // noise canceler, falling edge, clk/8
  TIMSK4 = _BV(ICIE4) | _BV(TOIE4);
  TCCR5A = 0;
  TCCR5B = _BV(ICNC5) | _BV(CS51);
  TIMSK5 = _BV(ICIE5) | _BV(TOIE5);
  TIFR4 = 0xFF;
  TIFR5 = 0xFF;
  interrupts();
}

static PulseInfo snapshot(Chan &c, bool tach) {
  uint16_t t, o;
  bool pend;
  uint32_t last, period, count;
  noInterrupts();
  t = tach ? TCNT4 : TCNT5;
  pend = tach ? (TIFR4 & _BV(TOV4)) : (TIFR5 & _BV(TOV5));
  o = c.ovf;
  last = c.last;
  period = c.period;
  count = c.count;
  const bool seen = c.seen;
  interrupts();
  PulseInfo p;
  p.count = count;
  if (!seen || !period) return p;
  uint32_t now = ((uint32_t)o << 16) | t;
  if (pend && t < 0x8000) now += 0x10000UL;
  p.period_us = period / 2;
  p.since_us = (now - last) / 2;
  return p;
}

PulseInfo capture_speed() { return snapshot(s_speed, false); }
PulseInfo capture_tach() { return snapshot(s_tach, true); }
