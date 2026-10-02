#pragma once

// CAN (TWAI) node for the DigiDash bus: one MCP2562 transceiver per board, 500 kbit/s.
// See docs/protocols/can_bus.md.
//
// Frames are sent single-shot (no hardware retry). Everything on this bus is periodic, so a lost
// frame is replaced by the next one; and with no transceiver fitted (bench) or nobody else on the
// bus, the controller does not sit retransmitting an unacknowledged frame forever.
//
// Build flags: ELC_CAN_TX_PIN / ELC_CAN_RX_PIN (-1 disables CAN on that board).
//
// The USB link (elc_usb_link.h) rides along: while a relay talks to the board, its frames go up the USB port too, and
// frames from the relay arrive through the same handler as the bus's. running() is true while either works.

#include <stddef.h>
#include <stdint.h>

#include "elc_can_codec.h"
#include "elc_can_ids.h"
#include "elc_chan.h"
#include "elc_usb_link.h"

#ifndef ELC_CAN_FW_VERSION
#define ELC_CAN_FW_VERSION 1
#endif

struct ElcCanStats {
  uint32_t tx_ok = 0;
  uint32_t tx_fail = 0;   // queue full or driver not running
  uint32_t rx = 0;
  uint32_t bus_off = 0;   // times the controller went bus-off (and was recovered)
  uint32_t tec = 0;       // transmit / receive error counters at the last poll
  uint32_t rec = 0;
  bool running = false;    // the CAN controller
};

/** Last heartbeat seen from each node (index = node number). */
struct ElcCanNodeInfo {
  bool seen = false;
  uint32_t last_ms = 0;
  ElcCanHeartbeat hb;
};

class ElcCan {
 public:
  using FrameHandler = void (*)(const ElcCanFrame &f, void *ctx);

  /** Installs and starts the driver. Returns false (and stays inert) when pins are -1 or the driver fails. */
  bool begin(int tx_pin, int rx_pin, uint8_t node, ElcCanNodeType type);

  /** The channel protocol (elc_chan.h): every received frame goes through it, its poll() runs from poll(), it sends
   *  through send(), and the heartbeat's flags bit0 says this node speaks it. Call after begin(). */
  void attachChannels(ElcChannels *ch, const ElcNetAnnounce &me);
  ElcChannels *channels() const { return _chan; }

  /** Every frame received is passed to this handler from poll() (loop context, not an ISR). */
  void onFrame(FrameHandler h, void *ctx) {
    _handler = h;
    _ctx = ctx;
  }

  /** Drains received frames, recovers from bus-off, sends the 1 Hz heartbeat. Call from loop(). */
  void poll();

  bool send(const ElcCanFrame &f);
  /** Free places in the transmit queue (0 when the driver is not running). Bulk senders (album art) fill
   *  only this many per pass, so a full queue is not counted as a failed send. */
  size_t txFree() const;
  /** The bus or the USB link carries frames. stats().running is the CAN controller alone. */
  bool running() const { return _stats.running || elc_usb_active(); }
  uint8_t node() const { return _node; }
  const ElcCanStats &stats() const { return _stats; }

  /** Node info from heartbeats; seen within max_age_ms. */
  bool nodeAlive(uint8_t node, uint32_t max_age_ms = 3000) const;
  const ElcCanNodeInfo &nodeInfo(uint8_t node) const { return _nodes[node % ELC_CAN_MAX_NODES]; }

  /** One-line summary for bench consoles. */
  void describe(char *out, size_t n) const;

 private:
  void heartbeat(uint32_t now);
  void deliver(const ElcCanFrame &f, uint32_t now);
  bool bus_send(const ElcCanFrame &f);

  FrameHandler _handler = nullptr;
  void *_ctx = nullptr;
  uint8_t _node = 0;
  ElcCanNodeType _type = ElcCanNodeType::Unknown;
  uint32_t _last_hb_ms = 0;
  uint32_t _recover_ms = 0;
  bool _recovering = false;
  ElcCanStats _stats;
  ElcCanNodeInfo _nodes[ELC_CAN_MAX_NODES];
  ElcChannels *_chan = nullptr;
};

/** Rev 1 carrier: strap value (0..2) from the two SLOT_ID straps, a grounded pin reads as 1. -1 pin = not strapped.
 *  The node is ELC_CAN_NODE_SLOT0 + that value. */
uint8_t elc_can_read_slot_id(int id0_pin, int id1_pin);

/** Rev 2 slot card: the slot (1 .. 6) from the slot ID pin (slot pin 11, 10k to 3.3 V on the card) on an ADC pin,
 *  0 when no main board pulls it down. The node is elc_can_node_for_card_slot() of it. */
uint8_t elc_can_read_card_slot(int adc_pin);
