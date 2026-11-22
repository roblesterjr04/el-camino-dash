#pragma once

// Power sequencing of the compute module (docs/hardware/ai_card.md, "Power sequence"). The ESP32 owns the module's
// 5 V buck: it is enabled only when the car, the menu and the temperature allow, shut down cleanly before it is cut,
// and power-cycled when it hangs. No Arduino dependencies: host-tested.

#include <stdint.h>

enum class AiPowerState : uint8_t { Off, Booting, Running, Stopping, Fault };

struct AiPowerIn {
  bool ignition_on = false;     // SYSTEM VEHICLE flag / POWER event
  bool enabled = true;          // the menu switch
  bool aux_present = false;     // the buck's input is up
  int16_t temp_x10 = 250;       // card temperature, 0.1 degC
  bool hello = false;           // the module's READY has been received since the buck came on
  bool alive_line = false;      // module -> ESP32 ALIVE line
  bool hung = false;            // PINGs unanswered
  bool shutdown_ack = false;    // SHUTDOWN_ACK received
};

struct AiPowerOut {
  bool buck_en = false;
  bool shutdown_req = false;    // the SHUTDOWN_REQ line
  bool send_shutdown = false;   // send a SHUTDOWN frame now (true once, on entering Stopping)
  uint8_t reason = 0;           // 1 ignition off, 2 disabled, 3 hot, 4 restart
  bool fault_raised = false;    // true once, on entering Fault (raise the advisory)
};

class AiPower {
 public:
  static const uint32_t kBootTimeoutMs = 90000;
  static const uint32_t kStopTimeoutMs = 20000;
  static const uint32_t kMinOffMs = 3000;
  static const uint32_t kFaultRetryMs = 600000;
  static const int kBootRetries = 3;
  static const int16_t kHotX10 = 800;   // stop at 80 degC
  static const int16_t kCoolX10 = 650;  // allow again under 65 degC

  /** Call often. The outputs are the levels to drive (and one-shot flags). */
  AiPowerOut poll(const AiPowerIn &in, uint32_t now_ms);
  AiPowerState state() const { return _s; }
  const char *name() const;
  bool hot() const { return _hot; }
  int failures() const { return _fails; }
  /** The module may be spoken to: Running. */
  bool running() const { return _s == AiPowerState::Running; }

 private:
  void enter(AiPowerState s, uint32_t now) { _s = s; _t0 = now; }
  AiPowerState _s = AiPowerState::Off;
  uint32_t _t0 = 0;
  uint32_t _off_since = 0;
  bool _hot = false;
  bool _restart = false;
  bool _started = false;
  int _fails = 0;
  uint8_t _reason = 0;
};

/** Card temperature, 0.1 degC, from an NTC divider: the NTC (r0 ohms at 25 degC, beta) to ground, r_fixed to vref, read in mV. */
int16_t ai_ntc_temp_x10(uint32_t mv, uint32_t vref_mv, uint32_t r_fixed, uint32_t r0, uint32_t beta);
