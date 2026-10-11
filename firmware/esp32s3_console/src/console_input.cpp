#include "console_input.h"

#include <Arduino.h>
#include <stdio.h>

#include "console_pins.h"

// ---- knobs: RetroSound 258A-style, two analog lines each (ConsoleBoardRS/README.md, "Knobs") ----
//
// Each jack's two lines have a 33k pull-up, so every resistance in the knob reads as a voltage. The encoder line
// steps open -> 82k -> 30k -> 47k -> open one way (the reverse the other), with detents at open and 30k: a two-bit
// Gray code, two steps per detent. The switch line reads open (none), 6.8k (ring left), 22k (ring right) or 0 (push).
// A task samples the four lines at 1 kHz and keeps each one's level band once it has read the same one
// CONSOLE_KNOB_STABLE_SAMPLES times running (the 10 nF settles within 2 ms; a step of a quickly turned knob lasts
// 15 ms or more). Only the encoder ever reads 1.45-2.65 V and only the switches below 1.45 V, so a line seen in the
// other's range swaps the pair (a reversed phone cord) without acting on the wrong reading.

enum Band : uint8_t { kOpen, k82, k47, k30, kRingRight, kRingLeft, kPush, kBands };

static Band band_of(int mv) {
  if (mv > 2650) return kOpen;      // 3.3 V (the ADC tops out near 3.1 V)
  if (mv > 2150) return k82;        // 2.35 V
  if (mv > 1755) return k47;        // 1.94 V
  if (mv > 1445) return k30;        // 1.57 V (82k ∥ 47k)
  if (mv > 940) return kRingRight;  // 1.32 V (22k)
  if (mv > 280) return kRingLeft;   // 0.56 V (6.8k)
  return kPush;                     // 0 Ω
}
static bool encoder_band(Band b) { return b == k82 || b == k47 || b == k30; }
static bool switch_band(Band b) { return b == kRingRight || b == kRingLeft || b == kPush; }
// the encoder's position in its four-step cycle (open, 82k, 30k, 47k)
static int enc_pos(Band b) { return b == k82 ? 1 : b == k30 ? 2 : b == k47 ? 3 : 0; }

struct Line {
  uint8_t pin;
  int mv;          // the last reading
  Band raw;        // the last reading's band
  uint8_t same;    // how many readings in a row it has held
  Band stable;     // the band once held long enough
};

enum Sw : uint8_t { kSwNone, kSwLeft, kSwRight, kSwPush };

struct Knob {
  Line line[2];   // contacts 2 and 3
  bool swapped;   // contact 3 carries the encoder
  bool reverse;
  int8_t pos;     // encoder position 0..3
  int8_t acc;     // steps since the last detent
  int32_t detents;
  Sw sw;          // the switch line now
  uint32_t swaps;
};

static Knob s_sel = {{{CONSOLE_SEL_ADC_A_PIN, 0, kOpen, 0, kOpen}, {CONSOLE_SEL_ADC_B_PIN, 0, kOpen, 0, kOpen}},
                     false, CONSOLE_SEL_REVERSE != 0, 0, 0, 0, kSwNone, 0};
static Knob s_vol = {{{CONSOLE_VOL_ADC_A_PIN, 0, kOpen, 0, kOpen}, {CONSOLE_VOL_ADC_B_PIN, 0, kOpen, 0, kOpen}},
                     false, CONSOLE_VOL_REVERSE != 0, 0, 0, 0, kSwNone, 0};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void line_sample(Line &l) {
  l.mv = analogReadMilliVolts(l.pin);
  const Band b = band_of(l.mv);
  if (b == l.raw) {
    if (l.same < 255) l.same++;
  } else {
    l.raw = b;
    l.same = 1;
  }
  if (l.same >= CONSOLE_KNOB_STABLE_SAMPLES) l.stable = l.raw;
}

static void knob_sample(Knob &k) {
  line_sample(k.line[0]);
  line_sample(k.line[1]);
  const Band enc = k.line[k.swapped ? 1 : 0].stable, sw = k.line[k.swapped ? 0 : 1].stable;
  if (switch_band(enc) || encoder_band(sw)) {  // the cord is the other way round: swap, and start again from here
    k.swapped = !k.swapped;
    k.swaps++;
    k.pos = (int8_t)enc_pos(k.line[k.swapped ? 1 : 0].stable);
    k.acc = 0;
    return;
  }
  const int p = enc_pos(enc);
  const int d = (p - k.pos + 4) & 3;  // 1 = one step on, 3 = one step back, 2 = a step missed (direction unknown)
  if (d == 1) k.acc++;
  if (d == 3) k.acc--;
  k.pos = (int8_t)p;
  if (d && (p == 0 || p == 2)) {  // on a detent: a whole click either way (one missed step still counts)
    if (k.acc) {
      const int dir = (k.acc > 0 ? 1 : -1) * (k.reverse ? -1 : 1);
      portENTER_CRITICAL(&s_mux);
      k.detents += dir;
      portEXIT_CRITICAL(&s_mux);
    }
    k.acc = 0;
  }
  k.sw = sw == kPush ? kSwPush : sw == kRingLeft ? kSwLeft : sw == kRingRight ? kSwRight : kSwNone;
}

static void knob_task(void *) {
  const TickType_t period = pdMS_TO_TICKS(1) ? pdMS_TO_TICKS(1) : 1;  // 1 ms at Arduino's 1 kHz tick
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    knob_sample(s_sel);
    knob_sample(s_vol);
    vTaskDelayUntil(&last, period);
  }
}

static void knob_begin(Knob &k) {
  for (Line &l : k.line) {
    pinMode(l.pin, INPUT);  // the 33k pull-up is on the board
    analogSetPinAttenuation(l.pin, ADC_11db);
    l.mv = analogReadMilliVolts(l.pin);
    l.raw = l.stable = band_of(l.mv);
    l.same = CONSOLE_KNOB_STABLE_SAMPLES;
  }
  if (switch_band(k.line[0].stable) || encoder_band(k.line[1].stable)) k.swapped = true;
  k.pos = (int8_t)enc_pos(k.line[k.swapped ? 1 : 0].stable);
}

static int knob_steps(Knob &k) {
  portENTER_CRITICAL(&s_mux);
  const int32_t d = k.detents;
  k.detents = 0;
  portEXIT_CRITICAL(&s_mux);
  return (int)d;
}

// ---- pushes and the right ring: clicks act on release so a hold can be told apart (the task has debounced them) ----

struct Key {
  bool stable_down;
  bool raw_down;
  uint32_t change_ms;
  uint32_t down_ms;
  bool held;
};

static Key s_sel_push, s_vol_push, s_ring_left, s_ring_right;

/** One switch: `down` is its level now. A click on release, a hold once at CONSOLE_HOLD_MS (its release does nothing). */
static void key_step(Key &k, bool down, uint32_t now_ms, bool *click, bool *hold) {
  if (down != k.raw_down) {
    k.raw_down = down;
    k.change_ms = now_ms;
  }
  if (k.raw_down != k.stable_down) {
    k.stable_down = k.raw_down;
    if (k.stable_down) {
      k.down_ms = now_ms;
      k.held = false;
    } else if (!k.held) {
      *click = true;
    }
  }
  if (k.stable_down && !k.held && now_ms - k.down_ms >= CONSOLE_HOLD_MS) {
    k.held = true;
    *hold = true;
  }
}

static Sw s_vol_ring = kSwNone;

/** The left ring acts once as it is flicked (the next flick needs it back in the middle first). */
static int ring_step(Sw now, Sw &last) {
  const Sw ring = now == kSwLeft || now == kSwRight ? now : kSwNone;
  const int r = ring != last && ring != kSwNone ? (ring == kSwRight ? 1 : -1) : 0;
  if (now != kSwPush) last = ring;  // a push (which hides the ring) does not count as letting go
  return r;
}

void console_input_begin() {
  analogReadResolution(12);
  knob_begin(s_sel);
  knob_begin(s_vol);
  s_sel_push.held = s_vol_push.held = s_ring_left.held = s_ring_right.held = true;
  xTaskCreatePinnedToCore(knob_task, "knobs", 3072, nullptr, 5, nullptr, 0);
}

void console_input_poll(ConsoleInputEvents &ev, uint32_t now_ms) {
  ev = ConsoleInputEvents();
  const Sw sel = s_sel.sw, vol = s_vol.sw;
  key_step(s_sel_push, sel == kSwPush, now_ms, &ev.right_click, &ev.right_hold);
  key_step(s_vol_push, vol == kSwPush, now_ms, &ev.left_click, &ev.left_hold);
  key_step(s_ring_left, sel == kSwLeft, now_ms, &ev.key_click[0], &ev.key_hold[0]);
  key_step(s_ring_right, sel == kSwRight, now_ms, &ev.key_click[1], &ev.key_hold[1]);
  ev.left_ring = ring_step(vol, s_vol_ring);
  ev.right_steps = knob_steps(s_sel);
  ev.left_steps = knob_steps(s_vol);
}

void console_input_describe(char *out, size_t n) {
  static const char *const kBand[kBands] = {"open", "82k", "47k", "30k", "ring-R", "ring-L", "push"};
  const Knob *ks[2] = {&s_vol, &s_sel};
  static const char *const kName[2] = {"left (volume)", "right (select)"};
  size_t o = 0;
  for (int i = 0; i < 2 && o < n; i++) {
    const Knob &k = *ks[i];
    const Line &e = k.line[k.swapped ? 1 : 0], &s = k.line[k.swapped ? 0 : 1];
    o += snprintf(out + o, n - o, "%s%s: encoder GPIO %u %d mV %s, switches GPIO %u %d mV %s%s (swapped %lu times)", i ? "\n" : "",
                  kName[i], e.pin, e.mv, kBand[e.stable], s.pin, s.mv, kBand[s.stable], k.swapped ? ", cord reversed" : "",
                  (unsigned long)k.swaps);
  }
}
