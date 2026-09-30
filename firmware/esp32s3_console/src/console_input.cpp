#include "console_input.h"

#include <Arduino.h>
#include <soc/gpio_reg.h>

#include "console_pins.h"

// ---- encoders: quadrature state table, run from a GPIO interrupt on every edge of A and B ----
//
// Index = previous AB << 2 | current AB. A legal one-step move counts +-1; no change or a skipped state (both lines
// changed at once, a bounce the RC did not catch) counts 0, so bounce cancels out instead of drifting.
static const int8_t kQuad[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

struct Encoder {
  uint8_t pin_a, pin_b;
  bool reverse;
  volatile uint8_t prev;
  volatile int32_t count;  // transitions
  int32_t taken;           // transitions already turned into detents (loop side)
};

static Encoder s_sel = {CONSOLE_SEL_A_PIN, CONSOLE_SEL_B_PIN, CONSOLE_SEL_REVERSE != 0, 0, 0, 0};
static Encoder s_vol = {CONSOLE_VOL_A_PIN, CONSOLE_VOL_B_PIN, CONSOLE_VOL_REVERSE != 0, 0, 0, 0};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Straight from the input registers: digitalRead is not guaranteed to be in IRAM.
static inline uint32_t IRAM_ATTR pin_level(uint8_t pin) {
  return pin < 32 ? (REG_READ(GPIO_IN_REG) >> pin) & 1u : (REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1u;
}

static void IRAM_ATTR on_edge(void *arg) {
  Encoder *e = (Encoder *)arg;
  const uint8_t cur = (uint8_t)((pin_level(e->pin_a) << 1) | pin_level(e->pin_b));
  portENTER_CRITICAL_ISR(&s_mux);
  e->count += kQuad[(e->prev << 2) | cur];
  e->prev = cur;
  portEXIT_CRITICAL_ISR(&s_mux);
}

static void encoder_begin(Encoder &e) {
  // External 10k pull-ups are fitted; the internal ones only matter on a bare dev board (bench).
  pinMode(e.pin_a, INPUT_PULLUP);
  pinMode(e.pin_b, INPUT_PULLUP);
  e.prev = (uint8_t)((digitalRead(e.pin_a) << 1) | digitalRead(e.pin_b));
  attachInterruptArg(e.pin_a, on_edge, &e, CHANGE);
  attachInterruptArg(e.pin_b, on_edge, &e, CHANGE);
}

/** Whole detents since the last call; the remainder stays for next time. */
static int encoder_steps(Encoder &e) {
  portENTER_CRITICAL(&s_mux);
  const int32_t count = e.count;
  portEXIT_CRITICAL(&s_mux);
  const int32_t d = (count - e.taken) / CONSOLE_ENC_STEPS_PER_DETENT;
  e.taken += d * CONSOLE_ENC_STEPS_PER_DETENT;
  return (int)(e.reverse ? -d : d);
}

// ---- keys: debounced by time, act on the press ----

struct Key {
  uint8_t pin;
  bool stable_down;
  bool raw_down;
  uint32_t change_ms;
};

static Key s_back = {CONSOLE_BACK_PIN, false, false, 0};
static Key s_view = {CONSOLE_VIEW_PIN, false, false, 0};
static Key s_sel_sw = {CONSOLE_SEL_SW_PIN, false, false, 0};
static Key s_vol_sw = {CONSOLE_VOL_SW_PIN, false, false, 0};

static void key_begin(Key &k) {
  pinMode(k.pin, INPUT_PULLUP);
  k.raw_down = k.stable_down = digitalRead(k.pin) == LOW;  // held at boot: no press until released
}

/** True once per press, after the line has been steady for the debounce time. */
static bool key_pressed(Key &k, uint32_t now_ms) {
  const bool down = digitalRead(k.pin) == LOW;
  if (down != k.raw_down) {
    k.raw_down = down;
    k.change_ms = now_ms;
  }
  if (k.raw_down != k.stable_down && now_ms - k.change_ms >= CONSOLE_DEBOUNCE_MS) {
    k.stable_down = k.raw_down;
    return k.stable_down;
  }
  return false;
}

void console_input_begin() {
  key_begin(s_back);
  key_begin(s_view);
  key_begin(s_sel_sw);
  key_begin(s_vol_sw);
  encoder_begin(s_sel);
  encoder_begin(s_vol);
}

/** Volume push: a short press acts on release, a hold (CONSOLE_HOLD_MS) acts once while still down. */
static void hold_key(Key &k, uint32_t now_ms, bool *press, bool *hold) {
  static uint32_t s_down_ms = 0;
  static bool s_held = false;
  const bool was = k.stable_down;
  const bool went_down = key_pressed(k, now_ms);
  if (went_down) {
    s_down_ms = now_ms;
    s_held = false;
  } else if (was && !k.stable_down && !s_held) {
    *press = true;
  }
  if (k.stable_down && !s_held && now_ms - s_down_ms >= CONSOLE_HOLD_MS) {
    s_held = true;
    *hold = true;
  }
}

void console_input_poll(ConsoleInputEvents &ev, uint32_t now_ms) {
  ev = ConsoleInputEvents();
  ev.back = key_pressed(s_back, now_ms);
  ev.view = key_pressed(s_view, now_ms);
  ev.sel_press = key_pressed(s_sel_sw, now_ms);
  hold_key(s_vol_sw, now_ms, &ev.vol_press, &ev.vol_hold);
  ev.sel_steps = encoder_steps(s_sel);
  ev.vol_steps = encoder_steps(s_vol);
}
