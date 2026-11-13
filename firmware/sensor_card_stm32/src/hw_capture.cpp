#include "hw_capture.h"

#include <Arduino.h>
#include <HardwareTimer.h>

#include "card_pins.h"

namespace {
const uint32_t kTickHz = 2000000;  // 0.5 us ticks, as rev A
PulseTrack s_speed(kSpeedMinTicks), s_tach(kTachMinTicks);
HardwareTimer *s_tim_tach = nullptr, *s_tim_speed = nullptr;

inline bool ovf_pending(TIM_TypeDef *t) { return (t->SR & TIM_SR_UIF) != 0; }

void tach_captured() { s_tach.captured((uint16_t)s_tim_tach->getCaptureCompare(1, TICK_COMPARE_FORMAT), ovf_pending(TIM1)); }
void tach_overflowed() { s_tach.overflowed(); }
void speed_captured() { s_speed.captured((uint16_t)s_tim_speed->getCaptureCompare(1, TICK_COMPARE_FORMAT), ovf_pending(TIM3)); }
void speed_overflowed() { s_speed.overflowed(); }

HardwareTimer *setup(TIM_TypeDef *inst, uint32_t pin, void (*on_capture)(), void (*on_overflow)()) {
  HardwareTimer *t = new HardwareTimer(inst);
  // input filter: 8 samples at the timer clock (rev A's noise canceler was 4 at 16 MHz)
  t->setMode(1, TIMER_INPUT_CAPTURE_FALLING, (pin_size_t)pin, FILTER_CKINT_N8);
  t->setPrescaleFactor(t->getTimerClkFreq() / kTickHz);
  t->setOverflow(0x10000, TICK_FORMAT);  // the full 16 bits, an overflow every 32.768 ms
  t->attachInterrupt(1, on_capture);
  t->attachInterrupt(on_overflow);
  t->resume();
  return t;
}

PulseInfo snapshot(const PulseTrack &c, TIM_TypeDef *inst) {
  noInterrupts();
  const uint16_t cnt = (uint16_t)inst->CNT;
  const bool pend = ovf_pending(inst);
  const PulseInfo p = c.snapshot(cnt, pend);
  interrupts();
  return p;
}
}  // namespace

void capture_begin() {
  // no internal pull-ups: the card has its own (the 22k on the tach opto, the 10k on the speed input)
  s_tim_tach = setup(TIM1, PIN_TACH, tach_captured, tach_overflowed);
  s_tim_speed = setup(TIM3, PIN_SPEED, speed_captured, speed_overflowed);
}

PulseInfo capture_speed() { return snapshot(s_speed, TIM3); }
PulseInfo capture_tach() { return snapshot(s_tach, TIM1); }
