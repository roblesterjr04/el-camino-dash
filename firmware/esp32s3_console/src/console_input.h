#pragma once

// Keys and encoders on the console board (ConsoleBoard/README.md): all active-low with 10k pull-ups and 10 nF.

#include <stdint.h>

struct ConsoleInputEvents {
  // Every switch: a click acts on release, a hold (CONSOLE_HOLD_MS) acts once while still down.
  bool left_click = false, left_hold = false;    // left knob (Volume encoder) push
  bool right_click = false, right_hold = false;  // right knob (Select encoder) push
  bool soft_left = false, soft_left_hold = false;    // left key under the OLED (Back)
  bool soft_right = false, soft_right_hold = false;  // right key (View)
  int left_steps = 0;   // detents, + = clockwise
  int right_steps = 0;
};

void console_input_begin();
/** Collects what happened since the last call. Call every loop pass (the keys are debounced here). */
void console_input_poll(ConsoleInputEvents &ev, uint32_t now_ms);
