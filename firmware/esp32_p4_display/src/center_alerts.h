#pragma once

#include <stddef.h>
#include <stdint.h>

#include <elc_alerts.h>

// The center's alert manager (docs/protocols/can_channels.md, ALERT): it collects every node's alerts, plays their
// chimes on the audio sink, tracks acknowledgement and sends SUMMARY (ElcAlertManager), and raises the center's own
// warnings from the sensor pack as node 0x01 (the "Center alerts" table in can_channels.md).
//
// Not yet: the menu screen's banner and list. Until then the driver acknowledges from the bench console
// (`alerts ack`). Started and polled by center_chan_begin() / center_chan_loop().

void center_alerts_begin();
void center_alerts_loop(uint32_t now_ms);
/** QUERY bit2 (or boot): resend the center's own DEFINEs. */
void center_alerts_describe(uint8_t what);

ElcAlertManager &center_alerts();

/** Console: `alerts` (the list), `alerts ack` (the top one), `alerts ack all`. False when not an alerts command. */
bool center_alerts_command(const char *line, char *reply, size_t n);
