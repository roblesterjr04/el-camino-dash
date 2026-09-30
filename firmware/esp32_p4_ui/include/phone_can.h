#pragma once

#include <stddef.h>
#include <stdint.h>

#include <elc_can_codec.h>

/**
 * Phone state from the audio bridge on CAN (docs/protocols/can_bus.md, "Phone") and the call card: an LVGL
 * overlay on the media screen, above the face and the menu, while a call exists or a pairing code waits. It
 * shows the caller and the call state (or the phone asking to pair and its code) and names what the console's
 * two soft keys do now. The displays take no phone input: the card swallows touches and does nothing with them.
 *
 * Loop contract: call phone_can_begin() once with this board's send hook and node; pass every CAN frame to
 * phone_can_frame(); call phone_can_poll() once per loop, before p4_screen_frame(). The card follows the gauge
 * theme (gauges_theme()) and is rebuilt when it changes. While this board's menu shows its Phone page (ui_menu.h),
 * a pairing request shows there (with Pair / Reject) instead of on the card.
 *
 * The menu's Phone page (pairing, the paired list) reads the state here and sends phone commands with
 * phone_can_send().
 */

/** send: how this board puts a frame on the bus; from_node: this board's CAN node (phone commands name it). */
void phone_can_begin(bool (*send)(const ElcCanFrame &f), uint8_t from_node);

/** Feed one received frame. True when it was the bridge's phone status, text or pairing. */
bool phone_can_frame(const ElcCanFrame &f);

/** Staleness, the bench demo and the card. show_here = this display is the media screen (and may cover its face). */
void phone_can_poll(bool show_here, uint32_t now_ms);

/** Phone status heard within the last 3 s (else the bridge is gone and no card shows). */
bool phone_can_heard();
const ElcCanPhoneStatus &phone_can_status();
/** Caller of the current call, "" until its text (with the status's caller serial) has arrived. */
const char *phone_can_caller();
/** Name of the connected phone ("" if none heard). */
const char *phone_can_phone_name();
/** The phone waiting for its code to be confirmed: name ("" until heard) and the six-digit code. */
const char *phone_can_pairing_name();
bool phone_can_passkey(uint32_t *code);
/** The bridge's slot (node - 0x10), learned from its status frames. */
uint8_t phone_can_slot();
/** Name of paired phone i (list order, most recently used first): "" until its text with the current list serial
 *  has arrived, or when i is not in the list. */
const char *phone_can_paired_name(uint8_t i);
/** Pairing window open (status flag), with the seconds left from the pairing frame (255 = until a phone pairs).
 *  *left is 255 too while no pairing frame has arrived yet. */
bool phone_can_pairing_window(uint8_t *left);
/**
 * Phone command (0x215) to the bridge's slot, from this board's node. Commands that name a paired index
 * (CONNECT, FORGET) carry the current list serial. False when no bridge is heard or there is no send hook.
 * With `phone demo` on, the command changes the demo state instead (pairing, confirm, connect, disconnect).
 */
bool phone_can_send(uint8_t command, uint8_t arg);
/** The card is on screen now. */
bool phone_can_card_showing();

/** One or more lines describing the phone state, for the `phone` console command. */
void phone_can_describe(char *out, size_t n);

/**
 * Console: `phone` (state) and `phone demo idle|discover|incoming|active|waiting|held|pair|off` (synthetic bridge
 * frames fed locally, refreshed so they do not go stale; `active` also takes `muted` and `onphone`; `idle` = a
 * connected phone and no call, `discover` = the pairing window open). Returns false when the
 * line is not a phone command; otherwise the reply (possibly several lines) is in `reply`.
 */
bool phone_can_command(const char *line, char *reply, size_t n);
