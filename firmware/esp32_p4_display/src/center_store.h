#pragma once

#include <stddef.h>
#include <stdint.h>

#include <elc_chan.h>

// The center's store on the STORE channel (docs/protocols/can_channels.md, STORE): any node on the bus PUTs, GETs and
// DELETEs its own named records, kept on the SD card under /elcamino/store/<product>_<node>/<key>. Without a card
// every request is answered NO_MEDIA. Needs chan_screen_begin() and center_sd_settings_begin() first.

void center_store_begin();
/** Every received channel transfer (center_chan passes them on); answers the STORE requests. */
void center_store_transfer(const ElcChTransfer &t);
/** Console: `store` (counters, and each node's records with their sizes). False when the line is not a store command. */
bool center_store_command(const char *line, char *reply, size_t n);
