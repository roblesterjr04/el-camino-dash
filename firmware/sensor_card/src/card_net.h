#pragma once

// The card's end of the channel protocol (docs/protocols/can_channels.md): transfers framed with the tail byte,
// reassembled per CAN ID, and NET discovery (ANNOUNCE, QUERY, IDENTIFY, RESTART). It does what ElcChannels does for the
// ESP32 boards, minus the table of every node on the bus: the card never needs another node's ANNOUNCE, and on 8 KB of
// RAM that table is the biggest thing in the class. Nothing here touches the hardware: host-tested.

#include <stddef.h>
#include <stdint.h>

#include "elc_can_codec.h"
#include "elc_chan.h"

/** Frames the card looks at: its channels (NET, UI, ALERT, SYSTEM), the GPS card's motion (0x223) for the speed
 *  calibration, and the center's heartbeat / status for the link LED. Everything else on the bus is dropped early. */
bool card_wants(uint16_t id);

class CardNet {
 public:
  typedef bool (*TxFn)(const ElcCanFrame &f);
  typedef uint8_t (*FreeFn)();
  struct Hooks {
    /** A complete transfer on a channel the card consumes (UI, ALERT, SYSTEM), already checked for this node. */
    void (*transfer)(const ElcChTransfer &t) = nullptr;
    /** QUERY bits 1-3 (sensor, alert and UI descriptions) asked for, or the boot descriptions. */
    void (*describe)(uint8_t what) = nullptr;
    /** IDENTIFY (seconds, 0 = stop) or RESTART addressed to this node. */
    void (*net_cmd)(uint8_t op, const uint8_t *p, size_t n) = nullptr;
  };

  void begin(uint8_t node, const ElcNetAnnounce &me, TxFn tx, FreeFn tx_free, const Hooks &hooks);
  /** Every frame heard (the bus and the USB link). */
  void feed(const ElcCanFrame &f, uint32_t now_ms);
  /** Boot ANNOUNCE, then answers to queries after (node x 8) ms. */
  void poll(uint32_t now_ms);

  /** One transfer on `base + node`. False (nothing sent) if the queue has no room for all of it, or it is too long. */
  bool send(uint16_t base, const uint8_t *payload, size_t len);
  bool send(uint16_t base, const ElcChWriter &w) { return w.ok() && send(base, w.data(), w.size()); }
  void announce();
  /** A description changed (units followed the center's PREFS): new generation, re-announce, describe again. */
  void descriptionsChanged();
  void setFactoryDefaults(bool on);
  uint8_t generation() const { return _me.generation; }
  uint8_t node() const { return _node; }
  uint32_t transfersIn() const { return _in; }
  uint32_t transfersOut() const { return _out; }
  uint32_t dropped() const { return _rx.dropped() + _send_fail; }

 private:
  uint8_t _node = 0;
  ElcNetAnnounce _me;
  TxFn _tx = nullptr;
  FreeFn _free = nullptr;
  Hooks _h;
  ElcChRx _rx;
  uint8_t _tid[32] = {0};
  bool _booted = false;
  uint8_t _pending_what = 0;
  uint32_t _pending_at = 0;
  uint32_t _in = 0, _out = 0, _send_fail = 0;
};
