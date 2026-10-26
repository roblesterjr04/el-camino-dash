#pragma once

// FDCAN1 (PA11 RX / PA12 TX, AF3) in classic CAN mode at the bus rate (ELC_CAN_BITRATE_KBPS, 500), single-shot: a
// frame is tried once, so a card alone on a bus never sits retransmitting (the elc_can rule). Frames queue in software
// (kTx) in front of the 3-deep hardware FIFO and leave in order. Receive is by interrupt (through the core's
// TIM16_FDCAN_IT0 handler) into a software ring, so a slow loop pass (a 512-byte FRAM write) loses nothing.
#include <stdint.h>

#include "elc_can_codec.h"

struct FdCanStats {
  uint32_t tx_sent = 0;     // handed to the controller
  uint32_t tx_ok = 0;       // acknowledged on the bus (TX event FIFO)
  uint32_t tx_dropped = 0;  // the software queue was full
  uint32_t rx = 0, rx_lost = 0, rx_over = 0, bus_off = 0;
};

class FdCan {
 public:
  /** Configures FDCAN1 and goes live. False if the HAL refused (a clock or timing problem, nothing to do with the bus). */
  bool begin();
  bool running() const { return _up; }
  /** Moves frames from the queue into the controller, counts events, recovers from bus-off. Every pass of loop(). */
  void service(uint32_t now_ms);
  bool send(const ElcCanFrame &f);
  uint8_t txFree() const { return (uint8_t)(kTx - _tx_n); }
  bool receive(ElcCanFrame &f);
  const FdCanStats &stats() const { return _st; }
  /** Error state: 0 active, 1 passive, 2 bus-off; and the last error code (0 none, 1 stuff, 2 form, 3 ack, 4 bit1,
   *  5 bit0, 6 crc). */
  uint8_t errorState() const { return _err_state; }
  uint8_t lastError() const { return _lec; }
  uint8_t rxErrors() const { return _rec; }
  uint8_t txErrors() const { return _tec; }

  /** Called from the receive interrupt (hw_fdcan.cpp). */
  void onRx();

  static const uint8_t kTx = 24, kRx = 32;

 private:
  bool _up = false;
  uint32_t _check_ms = 0, _off_ms = 0;
  uint8_t _err_state = 0, _lec = 0, _rec = 0, _tec = 0;
  ElcCanFrame _txq[kTx];
  uint8_t _tx_h = 0, _tx_n = 0;
  ElcCanFrame _rxq[kRx];
  volatile uint8_t _rx_h = 0, _rx_n = 0;
  FdCanStats _st;
};
