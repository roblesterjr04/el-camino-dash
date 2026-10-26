// Host tests for the rev B hardware layer's pure parts: adc_scale.h (12-bit, 3.3 V counts to rev A's 10-bit, 5 V
// scale; the level inputs) and pulse_track.h (capture timestamps extended past 16 bits, hold-off, stop detection).
#include <math.h>
#include <stdio.h>

#include "adc_scale.h"
#include "pulse_track.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define NEAR(a, b, tol) do { double _a = (a), _b = (b); if (fabs(_a - _b) > (tol)) { printf("FAIL %s:%d  %s=%g want %g\n", __FILE__, __LINE__, #a, _a, _b); g_fail++; } } while (0)

static void test_scale() {
  // a sender at half the bank value reads mid-scale on both cards
  NEAR(adc_sender_counts(4095), 1023.0, 1e-3);
  NEAR(adc_sender_counts(2048), 511.6, 0.1);
  NEAR(adc_sender_counts(0), 0.0, 1e-6);
  // 12.6 V at the slot through 100k / 22k: 2.272 V at the pin, 2820 counts of 4095; rev A read 465 counts
  // (2.272 / 5 x 1023), and cal::battery_volts(465) = 465 x 5 / 1023 x 122 / 22 = 12.6
  const uint16_t c12 = (uint16_t)lround(12.6 * 22.0 / 122.0 / 3.3 * 4095.0);
  const float reva = adc_batt_counts(c12);
  NEAR(reva * (5.0 / 1023.0) * (122.0 / 22.0), 12.6, 0.03);
  NEAR(adc_slot_volts(c12, 100.0f, 22.0f), 12.6, 0.03);
  NEAR(adc_pin_volts(4095), 3.3, 1e-3);
}

static uint16_t counts_for_slot_volts(float v, float top, float bottom) {
  return (uint16_t)lround(v * bottom / (top + bottom) / 3.3 * 4095.0);
}

static void test_levels() {
  LevelInput ign(33.0f, 10.0f, 7.5f, 3.75f);
  CHECK(!ign.update(counts_for_slot_volts(0.0f, 33, 10)));
  CHECK(!ign.update(counts_for_slot_volts(7.0f, 33, 10)));   // cranking sag below the on threshold, still off
  CHECK(ign.update(counts_for_slot_volts(7.6f, 33, 10)));
  CHECK(ign.update(counts_for_slot_volts(5.0f, 33, 10)));    // hysteresis: stays on down to 3.75 V
  CHECK(ign.update(counts_for_slot_volts(14.5f, 33, 10)));
  CHECK(!ign.update(counts_for_slot_volts(3.0f, 33, 10)));
  LevelInput alt(39.0f, 10.0f, 9.6f, 4.8f);
  CHECK(!alt.update(counts_for_slot_volts(2.0f, 39, 10)));   // lamp on: the L terminal is pulled low
  CHECK(alt.update(counts_for_slot_volts(13.8f, 39, 10)));   // charging
  CHECK(alt.update(counts_for_slot_volts(6.0f, 39, 10)));
  CHECK(!alt.update(counts_for_slot_volts(4.0f, 39, 10)));
  // the alternator and battery dividers keep 16 V under the 3.3 V reference; the ignition one reaches it at 14.2 V
  // and relies on its clamp above that (the ADC then reads full scale, which is still "on")
  CHECK(16.0f * 10.0f / 49.0f < 3.3f);
  CHECK(16.0f * 22.0f / 122.0f < 3.3f);
  CHECK(ign.update(4095));
}

/** Drives a track with pulses every `period` ticks from `t0`, through the 16-bit timer's overflows. */
struct Sim {
  PulseTrack &t;
  uint32_t now = 0;  // absolute ticks
  explicit Sim(PulseTrack &track) : t(track) {}
  void advance_to(uint32_t abs) {
    while ((now >> 16) < (abs >> 16)) {
      now = ((now >> 16) + 1) << 16;
      t.overflowed();
    }
    now = abs;
  }
  void pulse_at(uint32_t abs, bool late_overflow_irq = false) {
    if (late_overflow_irq) {
      // the overflow happened but its interrupt has not run when the capture is handled
      while ((now >> 16) + 1 < (abs >> 16)) {
        now = ((now >> 16) + 1) << 16;
        t.overflowed();
      }
      now = abs;
      t.captured((uint16_t)abs, true);
      t.overflowed();
    } else {
      advance_to(abs);
      t.captured((uint16_t)abs, false);
    }
  }
  PulseInfo snap() const { return t.snapshot((uint16_t)now, false); }
};

static void test_pulses() {
  PulseTrack sp(kSpeedMinTicks);
  Sim s(sp);
  CHECK(s.snap().period_us == 0 && s.snap().count == 0);
  s.pulse_at(1000);
  CHECK(s.snap().count == 1 && s.snap().period_us == 0);  // one pulse: no period yet
  s.pulse_at(46000);  // 45000 ticks = 22.5 ms apart (44.4 Hz, 20 mph at 8000 p/mi)
  PulseInfo p = s.snap();
  CHECK(p.count == 2);
  CHECK(p.period_us == 22500);
  s.advance_to(46000 + 20000);
  CHECK(s.snap().since_us == 10000);
  s.pulse_at(91000);  // across the first overflow, the overflow interrupt having run
  CHECK(s.snap().period_us == 22500 && s.snap().count == 3);
  // 136000 is 4928 ticks into the third 65536-tick epoch: the capture is handled before the overflow interrupt
  s.pulse_at(136000, true);
  CHECK(s.snap().period_us == 22500);
  CHECK(s.snap().count == 4);
  s.pulse_at(181000);
  CHECK(s.snap().period_us == 22500 && s.snap().count == 5);
  // hold-off: a bounce 200 us after the pulse is ignored
  s.pulse_at(181000 + 400);
  CHECK(s.snap().count == 5 && s.snap().period_us == 22500);
  // stopped: no pulse for over 100 overflows (3.3 s)
  s.advance_to(181000 + 102 * 65536);
  CHECK(s.snap().period_us == 0);
  CHECK(s.snap().count == 5);
  // and running again
  s.pulse_at(s.now + 10);
  s.pulse_at(s.now + 2000);
  CHECK(s.snap().period_us == 1000);
}

static void test_tach_holdoff() {
  PulseTrack t(kTachMinTicks);
  Sim s(t);
  // a V8 at 6000 rpm: 400 pulses a second, 2.5 ms apart = 5000 ticks; points bounce 300 us after each
  uint32_t at = 500;
  for (int i = 0; i < 50; i++) {
    s.pulse_at(at);
    s.pulse_at(at + 600);  // the bounce
    at += 5000;
  }
  const PulseInfo p = s.snap();
  CHECK(p.count == 50);
  CHECK(p.period_us == 2500);
  // the engine formula: (60e6 / ppr) / period_us
  CHECK((60000000UL / 4) / p.period_us == 6000);
}

int main() {
  test_scale();
  test_levels();
  test_pulses();
  test_tach_holdoff();
  if (g_fail) {
    printf("%d failure(s)\n", g_fail);
    return 1;
  }
  printf("hw logic: all tests passed\n");
  return 0;
}
