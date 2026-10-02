#pragma once

#include <stddef.h>
#include <stdint.h>

#include <elc_can.h>
#include <elc_chan.h>
#include <elc_chan_msg.h>

/**
 * A round screen's side of the channel protocol (docs/protocols/can_channels.md): its node announces itself (a
 * display, product 0x0001), and every transfer it hears goes to the modules that use it: the accessory pages
 * (accessory_can.h), SYSTEM state, and on the center the LOG lines (printed on its console with the sender's name).
 *
 * Loop contract: chan_screen_begin() once, after ElcCan::begin(); chan_screen_poll() once per loop after
 * ElcCan::poll().
 */

/** screen: ELC_CAN_SCREEN_LEFT / CENTER / RIGHT. Attaches the channels to `can`. */
void chan_screen_begin(ElcCan &can, uint8_t screen);
void chan_screen_poll(uint32_t now_ms);
/** The board's channel node (nullptr before begin). */
ElcChannels *chan_screen();

/** Called with every transfer after the built-in handling (the center adds its own: alerts, settings backup). */
void chan_screen_on_transfer(void (*fn)(const ElcChTransfer &t, void *ctx), void *ctx);
/** Called when a node asks for this screen's descriptions (QUERY bits 1-3) and at boot. */
void chan_screen_on_describe(void (*fn)(uint8_t what, void *ctx), void *ctx);

/** The last SYSTEM state from the center; false until one arrived (age in ms of the last VEHICLE frame in *age). */
bool chan_screen_vehicle(ElcSysVehicle *out, uint32_t *age_ms);
bool chan_screen_prefs(ElcSysPrefs *out);

/**
 * Console: `chan` (the channel node: transfers, the announced nodes, the accessory items) and `chan query` (ask every
 * node for its announce and descriptions). False when the line is not a chan command.
 */
bool chan_screen_command(const char *line, char *reply, size_t n);
