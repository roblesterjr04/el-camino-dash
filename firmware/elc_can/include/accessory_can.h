#pragma once

#include <stddef.h>
#include <stdint.h>

#include <elc_chan.h>
#include <elc_chan_msg.h>

/**
 * Accessory menu pages from the UI channel (docs/protocols/can_channels.md, UI): every node's items, their values and
 * texts, kept on every screen (any of them can be the menu screen). The menu's Accessories page (ui_menu.h) reads
 * them here and changes them with accessory_can_set(); while one of its pages shows, accessory_can_view() keeps the
 * node's live items coming (VIEW every 1 s).
 *
 * Loop contract: accessory_can_begin() once with the board's ElcChannels; pass every complete transfer to
 * accessory_can_transfer(); call accessory_can_poll() once per loop.
 */

static const int kAccMaxNodes = 8;
static const int kAccMaxItems = 64;

struct AccItem {
  ElcUiItem d;
  bool defined = false;
  bool has_value = false;
  int32_t value = 0;
  bool has_text = false;
  char text[65] = {0};
  // the last SET from this screen and its RESULT
  bool pending = false;
  uint32_t pending_ms = 0;
  uint8_t result = 0;
  uint32_t result_ms = 0;
};

void accessory_can_begin(ElcChannels *ch);
void accessory_can_transfer(const ElcChTransfer &t, uint32_t now_ms);
void accessory_can_poll(uint32_t now_ms);

/** Nodes with at least one item, in node order. Returns how many were written. */
int accessory_can_nodes(uint8_t *out, int max);
/** The node's ANNOUNCE name, or "Node 0x15" until it has announced. */
const char *accessory_can_name(uint8_t node);
/** The node's items under `parent` (ELC_UI_TOP = its top page), in item order; advanced ones only when asked. */
int accessory_can_items(uint8_t node, uint8_t parent, bool advanced, const AccItem **out, int max);
const AccItem *accessory_can_item(uint8_t node, uint8_t item);
/** The value as the menu shows it: "94.7 MHz", "On", the chosen option, the text, "--" before any value. */
void accessory_can_value_text(const AccItem &it, char *out, size_t n);

/** SET to the node (min..max, on the step, not read-only). False when it could not be sent. */
bool accessory_can_set(uint8_t node, uint8_t item, int32_t value);
/** The page of `node` under `group` shows (ELC_UI_TOP for its top page); node 0 = no accessory page shows. */
void accessory_can_view(uint8_t node, uint8_t group);

/** Changes whenever anything the node's pages show changes (for rebuilding a page). */
uint32_t accessory_can_signature(uint8_t node);
/** Item descriptions or values received in total (for the bench console). */
void accessory_can_describe(char *out, size_t n);
