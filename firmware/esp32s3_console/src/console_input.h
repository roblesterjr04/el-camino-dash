#pragma once

// The two RetroSound knobs on the console board (ConsoleBoardRS/README.md), and an IR remote (Apple, Sirius) on the optional IR
// receiver (J6).

#include <stddef.h>
#include <stdint.h>

#include "ir_remote.h"

struct ConsoleInputEvents {
  // Pushes and the right ring: a click acts on release, a hold (CONSOLE_HOLD_MS) acts once while still down.
  bool left_click = false, left_hold = false;    // left knob (Volume) push
  bool right_click = false, right_hold = false;  // right knob (Select) push
  bool key_click[2] = {false, false}, key_hold[2] = {false, false};  // the right ring = the soft keys: 0 left, 1 right
  int left_ring = 0;                             // the left ring flicked: -1 left, +1 right (on the flick, once)
  int left_steps = 0, right_steps = 0;           // detents, + = clockwise
  RemoteEvents remote;                           // the IR remote
};

void console_input_begin();
/** Collects what happened since the last call. Call every loop pass. */
void console_input_poll(ConsoleInputEvents &ev, uint32_t now_ms);
/** The knob lines now (mV, which line is which, what they read), for the serial console. */
void console_input_describe(char *out, size_t n);
/** On: print every knob line's band changes (raw and debounced, with mV) and each step and click decoded. */
void console_input_knob_log(bool on);
/** The IR receiver: the last frame heard (any remote's) and how many, for the serial console. */
void console_input_describe_ir(char *out, size_t n);
/** On: print every IR burst's marks and spaces (us) and what it decoded to. */
void console_input_ir_log(bool on);
