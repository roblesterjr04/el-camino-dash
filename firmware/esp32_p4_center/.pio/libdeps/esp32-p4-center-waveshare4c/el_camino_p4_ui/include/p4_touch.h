#pragma once

#include <stdint.h>

// Waveshare ESP32-P4 round MIPI kits use GT911 @ 0x5D/0x14 (same SDA/SCL as PMIC). Some clones use CST816 @ 0x15.
// Auto-detect on begin(); no separate probe needed.

void p4_touch_begin(int sda = 7, int scl = 8);

// True while finger is down (GT911 or CST816).
bool p4_touch_finger_down();

// True once per finger lift; (x,y) last position while down.
bool p4_touch_poll_release(int16_t *x, int16_t *y);
