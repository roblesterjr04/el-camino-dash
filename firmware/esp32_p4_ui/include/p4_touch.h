#pragma once

#include <stdint.h>

// Waveshare ESP32-P4 round MIPI kits use GT911 @ 0x5D/0x14 (same SDA/SCL as PMIC). Some clones use CST816 @ 0x15.
// Auto-detect on begin(); no separate probe needed.

void p4_touch_begin(int sda = 7, int scl = 8);

// True while finger is down (GT911 or CST816), debounced: a lift counts after 40 ms of "up" reports.
bool p4_touch_finger_down();

// Current finger position while down (or last sampled); *down = finger down.
void p4_touch_sample(int16_t *x, int16_t *y, bool *down);

// True once per finger lift; (x,y) last position while down.
bool p4_touch_poll_release(int16_t *x, int16_t *y);

/** Read counters since boot: controller reads, reads with no new report, I2C errors, and brief lifts
 *  (shorter than the release debounce) that were ignored. */
struct P4TouchStats {
  uint32_t reads;
  uint32_t no_news;
  uint32_t errors;
  uint32_t glitches_ignored;
};
P4TouchStats p4_touch_stats();
