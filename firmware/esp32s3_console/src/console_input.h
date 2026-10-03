#pragma once

// The four soft keys and the two RetroSound knobs on the console board (ConsoleBoard4Key/README.md).

#include <stddef.h>
#include <stdint.h>

struct ConsoleInputEvents {
  // Keys and knob pushes: a click acts on release, a hold (CONSOLE_HOLD_MS) acts once while still down.
  bool key_click[4] = {false, false, false, false}, key_hold[4] = {false, false, false, false};  // top to bottom
  bool left_click = false, left_hold = false;    // left knob (Volume) push
  bool right_click = false, right_hold = false;  // right knob (Select) push
  int left_ring = 0, right_ring = 0;             // the outer ring flicked: -1 left, +1 right (on the flick, once)
  int left_steps = 0, right_steps = 0;           // detents, + = clockwise
};

void console_input_begin();
/** Collects what happened since the last call. Call every loop pass (the keys are debounced here). */
void console_input_poll(ConsoleInputEvents &ev, uint32_t now_ms);
/** The knob lines now (mV, which line is which, what they read), for the serial console. */
void console_input_describe(char *out, size_t n);
