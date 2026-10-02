#pragma once

#include <stdint.h>

// The center's own traffic on the channel protocol (docs/protocols/can_channels.md): the core sensors on SENSOR
// (node 0x01, the "Core sensors" table: DEFINEs at boot and on QUERY, VALUEs at each sensor's period) and SYSTEM
// (TIME every 10 s, VEHICLE at 5 Hz, PREFS every 5 s and on change). Needs chan_screen_begin() first.

void center_chan_begin();
void center_chan_loop(uint32_t now_ms);
