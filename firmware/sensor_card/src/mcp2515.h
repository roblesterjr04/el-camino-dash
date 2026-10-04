#pragma once

// MCP2515 CAN controller on SPI, polled (INT is not needed). 500 kbit/s, single-shot: a frame is tried once, so a card
// alone on a bus never sits retransmitting (the elc_can rule). A lost arbitration is retried up to 4 times; frames
// leave in the order they were queued, because only one frame is ever in the controller.
#include <stdint.h>

#include "elc_can_codec.h"

struct Mcp2515Stats {
  uint32_t tx_ok = 0, tx_fail = 0, tx_dropped = 0, rx = 0, rx_over = 0, bus_off = 0;
};

class Mcp2515 {
 public:
  /** Finds the chip (on `cs`, or on each of the candidates in turn when cs is 0), configures it and goes live.
   *  `mhz` is its crystal, 16 (the card) or 8. False if nothing answered. */
  bool begin(uint8_t cs, uint8_t mhz);
  bool running() const { return _up; }
  uint8_t cs() const { return _cs; }
  /** Moves frames in and out of the chip. Call every pass of loop(). */
  void service(uint32_t now_ms);
  bool send(const ElcCanFrame &f);
  uint8_t txFree() const { return (uint8_t)(kTx - _tx_n); }
  bool receive(ElcCanFrame &f);
  const Mcp2515Stats &stats() const { return _st; }
  uint8_t errorFlags() const { return _eflg; }

 private:
  bool probe(uint8_t cs);
  bool configure();
  uint8_t readReg(uint8_t a);
  void writeReg(uint8_t a, uint8_t v);
  void bitModify(uint8_t a, uint8_t mask, uint8_t v);
  uint8_t readStatus();
  void load(const ElcCanFrame &f);
  void readRx(uint8_t instr);
  static const uint8_t kTx = 24, kRx = 8;
  uint8_t _cs = 0, _mhz = 16;
  bool _up = false, _inflight = false;
  uint8_t _attempts = 0;
  uint32_t _sent_ms = 0, _check_ms = 0;
  uint8_t _eflg = 0;
  ElcCanFrame _cur;
  ElcCanFrame _txq[kTx];
  uint8_t _tx_h = 0, _tx_n = 0;
  ElcCanFrame _rxq[kRx];
  uint8_t _rx_h = 0, _rx_n = 0;
  Mcp2515Stats _st;
};
