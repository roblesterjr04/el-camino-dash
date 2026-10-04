#pragma once

// What the card publishes on the channels (docs/protocols/can_channels.md): its SENSOR readings (the center's core
// sensors 0-10 plus 11 battery voltage, same numbers, quantities and exponents), the one calibration ALERT, and the
// descriptions (SENSOR DEFINE, ALERT DEFINE, UI ITEM and the settings' VALUEs) sent at boot and on QUERY, one transfer
// at a time so the bus is never flooded. No hardware: host-tested.

#include <stdint.h>

#include "calib.h"
#include "card_net.h"
#include "card_ui.h"
#include "engine.h"

class CardChan {
 public:
  struct Ctx {
    CardNet *net = nullptr;
    CardUi *ui = nullptr;
    const EngineOut *out = nullptr;
    const cal::Calibration *cal = nullptr;
  };
  void begin(const Ctx &c, uint32_t now);
  /** QUERY bits 1-3 (ELC_NET_Q_*), or the boot descriptions. */
  void describe(uint8_t what);
  /** Pass the current time; sends what is due. Call every pass of loop(). */
  void poll(uint32_t now);
  /** An ALERT ACK for the card's alert. Nothing to do but note it. */
  uint32_t sensorValuesSent() const { return _values_sent; }

  static const uint8_t kSensors = 12;

 private:
  void sendValues(uint32_t now);
  void sendAlertState(uint32_t now, bool force);
  bool wantAlert() const;
  Ctx _c;
  uint8_t _pending = 0;  // ELC_NET_Q_SENSORS | _ALERTS | _UI still to send
  uint8_t _stage = 0;    // 0 idle, else the stage running
  uint16_t _idx = 0;
  uint32_t _last_send = 0;
  uint32_t _next[kSensors] = {0};
  uint32_t _alert_ms = 0;
  bool _alert_on = false;
  uint32_t _values_sent = 0;
};
