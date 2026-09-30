#pragma once

// Keys and encoders on the console board (ConsoleBoard/README.md): all active-low with 10k pull-ups and 10 nF.

#include <stdint.h>

struct ConsoleInputEvents {
  bool back = false;       // left soft key (pressed this pass)
  bool view = false;       // right soft key
  bool sel_press = false;
  bool vol_press = false;  // short press (on release)
  bool vol_hold = false;   // held CONSOLE_HOLD_MS
  int sel_steps = 0;       // detents, + = clockwise
  int vol_steps = 0;
};

void console_input_begin();
/** Collects what happened since the last call. Call every loop pass (the keys are debounced here). */
void console_input_poll(ConsoleInputEvents &ev, uint32_t now_ms);
