#pragma once

// The screen while a firmware update arrives over CAN (elc_fw.h): a black panel over everything with the progress, so
// a screen never looks like a frozen gauge. It shows while the image arrives and is written, says "restarting" when it
// is done, and shows a failure for a few seconds before the gauges come back.

#include <stdint.h>

class ElcFwTarget;

/** Call every loop pass after the gauges are updated, before p4_screen_frame(). True while the overlay covers the
 *  screen (the caller may skip its own drawing). */
bool fw_overlay_poll(const ElcFwTarget *t, uint32_t now_ms);
