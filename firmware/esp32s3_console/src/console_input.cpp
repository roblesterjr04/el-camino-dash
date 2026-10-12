#include "console_input.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>
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

// ---- the IR receiver (J6): the Apple or Sirius remote (ir_remote.cpp) ----
//
// The TSOP's output is low while it hears the 38 kHz carrier (a mark). An interrupt on every edge stores how long the
// level that just ended lasted; the loop decodes them (ir_remote.cpp). The ISR runs with the flash cache off too
// (Arduino installs the GPIO ISR service IRAM-only), so it reads the pin and the clock from IRAM-safe places.

static constexpr uint32_t kIrMark = 0x80000000u;
static volatile uint32_t s_ir_buf[256];  // the high bit = a mark, the rest its length in us
static volatile uint8_t s_ir_head = 0;    // the ISR writes here
static volatile uint8_t s_ir_tail = 0;   // the loop reads from here
static volatile uint32_t s_ir_last_us = 0;
static volatile uint32_t s_ir_edges = 0;  // every edge, decoded or not
static NecDecoder s_nec;
static IrRemote s_remote(CONSOLE_HOLD_MS);
static uint32_t s_ir_code = 0, s_ir_frames = 0, s_ir_repeats = 0;

#if CONSOLE_IR_PIN >= 0
static void IRAM_ATTR ir_isr() {
  const uint32_t t = (uint32_t)esp_timer_get_time();
  const uint32_t in = CONSOLE_IR_PIN < 32 ? REG_READ(GPIO_IN_REG) >> CONSOLE_IR_PIN
                                          : REG_READ(GPIO_IN1_REG) >> (CONSOLE_IR_PIN - 32);
  const uint32_t d = t - s_ir_last_us;
  s_ir_last_us = t;
  s_ir_edges = s_ir_edges + 1;
  const uint8_t h = s_ir_head;
  if ((uint8_t)(h + 1) == s_ir_tail) return;  // full: the loop is far behind, drop it (the decoder starts again)
  s_ir_buf[h] = ((in & 1) ? kIrMark : 0) | (d < kIrMark ? d : kIrMark - 1);  // high now: a mark has just ended
  s_ir_head = (uint8_t)(h + 1);
}
#endif

// the LED: blue for 60 ms after any IR edge, green for 150 ms after a known remote's frame or repeat (green wins)
static uint32_t s_led_edges = 0, s_led_ir_ms = 0, s_led_known_ms = 0;
static uint8_t s_led_shown = 0;  // 0 off, 1 blue, 2 green

static void ir_led(uint32_t now_ms) {
#if CONSOLE_IR_LED_PIN >= 0
  const uint32_t edges = s_ir_edges;
  if (edges != s_led_edges) {
    s_led_edges = edges;
    s_led_ir_ms = now_ms ? now_ms : 1;
  }
  const uint8_t want = s_led_known_ms && now_ms - s_led_known_ms < 150 ? 2 : s_led_ir_ms && now_ms - s_led_ir_ms < 60 ? 1 : 0;
  if (want == s_led_shown) return;
  s_led_shown = want;
  rgbLedWrite(CONSOLE_IR_LED_PIN, 0, want == 2 ? 40 : 0, want == 1 ? 40 : 0);
#else
  (void)now_ms;
#endif
}

// `ir log on`: every burst the receiver puts out, as M (mark) and S (space) lengths in us, one line per burst (a space
// over 20 ms ends it), and what it decoded to.
static bool s_ir_log = false;
static char s_ir_line[480];
static size_t s_ir_line_n = 0;
static uint32_t s_ir_line_edges = 0;

static void ir_log_flush() {
  if (!s_ir_line_n) return;
  Serial.printf("ir: %lu edges: %s\n", (unsigned long)s_ir_line_edges, s_ir_line);
  s_ir_line_n = 0;
  s_ir_line_edges = 0;
}

static void ir_log_edge(bool mark, uint32_t us) {
  if (!mark && us > 20000) {  // the gap between bursts
    ir_log_flush();
    return;
  }
  s_ir_line_edges++;
  if (s_ir_line_n + 12 >= sizeof(s_ir_line)) {
    if (s_ir_line_n + 4 < sizeof(s_ir_line)) s_ir_line_n += snprintf(s_ir_line + s_ir_line_n, sizeof(s_ir_line) - s_ir_line_n, " ...");
    return;
  }
  s_ir_line_n += snprintf(s_ir_line + s_ir_line_n, sizeof(s_ir_line) - s_ir_line_n, "%s%c%lu", s_ir_line_n ? " " : "",
                          mark ? 'M' : 'S', (unsigned long)us);
}

void console_input_ir_log(bool on) {
  s_ir_log = on;
  s_ir_line_n = 0;
  s_ir_line_edges = 0;
}

static void ir_poll(RemoteEvents &ev, uint32_t now_ms) {
  while (s_ir_tail != s_ir_head) {
    const uint32_t e = s_ir_buf[s_ir_tail];
    s_ir_tail = (uint8_t)(s_ir_tail + 1);
    if (s_ir_log) ir_log_edge((e & kIrMark) != 0, e & ~kIrMark);
    uint32_t code = 0;
    switch (s_nec.feed((e & kIrMark) != 0, e & ~kIrMark, &code)) {
      case NecDecoder::kFrame:
        if (s_ir_log) {
          ir_log_flush();
          Serial.printf("ir: NEC frame 0x%08lX: %s\n", (unsigned long)code,
                        remote_key(code) == RemoteKey::None ? "not a known remote" : remote_key_name(remote_key(code)));
        }
        s_ir_code = code;
        s_ir_frames++;
        if (remote_key(code) != RemoteKey::None) s_led_known_ms = now_ms ? now_ms : 1;
        s_remote.frame(code, now_ms);
        break;
      case NecDecoder::kRepeat:
        if (s_ir_log) {
          ir_log_flush();
          Serial.println("ir: NEC repeat");
        }
        s_ir_repeats++;
        if (s_remote.down() != RemoteKey::None) s_led_known_ms = now_ms ? now_ms : 1;
        s_remote.repeat(now_ms);
        break;
      default: break;
    }
  }
  // the last burst ends with the line idle, which no edge reports
  if (s_ir_log && s_ir_line_n && (uint32_t)esp_timer_get_time() - s_ir_last_us > 20000) ir_log_flush();
  s_remote.poll(ev, now_ms);
  ir_led(now_ms);
}

void console_input_describe_ir(char *out, size_t n) {
  if (CONSOLE_IR_PIN < 0) {
    snprintf(out, n, "ir: no receiver (CONSOLE_IR_PIN -1)");
    return;
  }
  snprintf(out, n, "ir: GPIO %d %s, %lu edges, %lu frames, %lu repeats, last 0x%08lX (%s), down: %s", CONSOLE_IR_PIN,
           digitalRead(CONSOLE_IR_PIN) ? "high (idle)" : "LOW (hearing IR, or no receiver pull-up)",
           (unsigned long)s_ir_edges, (unsigned long)s_ir_frames, (unsigned long)s_ir_repeats, (unsigned long)s_ir_code,
           s_ir_frames ? (remote_key(s_ir_code) == RemoteKey::None ? "not a known remote" : remote_key_name(remote_key(s_ir_code)))
                       : "none yet",
           remote_key_name(s_remote.down()));
}

void console_input_begin() {
#if CONSOLE_IR_PIN >= 0
  pinMode(CONSOLE_IR_PIN, INPUT_PULLUP);  // the TSOP has its own pull-up; this one keeps an empty J6 quiet
  s_ir_last_us = (uint32_t)esp_timer_get_time();
  attachInterrupt(digitalPinToInterrupt(CONSOLE_IR_PIN), ir_isr, CHANGE);
#endif
#if CONSOLE_IR_LED_PIN >= 0
  rgbLedWrite(CONSOLE_IR_LED_PIN, 0, 0, 0);
#endif
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
  ir_poll(ev.remote, now_ms);
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
