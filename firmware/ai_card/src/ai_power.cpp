#include "ai_power.h"

#include <math.h>

const char *AiPower::name() const {
  switch (_s) {
    case AiPowerState::Off: return "off";
    case AiPowerState::Booting: return "booting";
    case AiPowerState::Running: return "running";
    case AiPowerState::Stopping: return "stopping";
    case AiPowerState::Fault: return "fault";
  }
  return "?";
}

AiPowerOut AiPower::poll(const AiPowerIn &in, uint32_t now) {
  AiPowerOut o;
  if (!_started) {  // first call: the buck has been off since reset
    _started = true;
    _off_since = now - kMinOffMs;
  }
  // thermal latch with hysteresis
  if (in.temp_x10 >= kHotX10) _hot = true;
  else if (in.temp_x10 < kCoolX10) _hot = false;

  const bool want = in.ignition_on && in.enabled && in.aux_present && !_hot;
  auto why_not = [&]() -> uint8_t { return !in.ignition_on ? 1 : !in.enabled ? 2 : _hot ? 3 : 1; };

  switch (_s) {
    case AiPowerState::Off:
      if (want && now - _off_since >= kMinOffMs) enter(AiPowerState::Booting, now);
      break;
    case AiPowerState::Booting:
      if (!in.aux_present || !in.ignition_on) {  // nothing to shut down cleanly yet: just cut
        _fails = 0;
        _off_since = now;
        enter(AiPowerState::Off, now);
      } else if (in.hello && in.alive_line) {
        _fails = 0;
        enter(AiPowerState::Running, now);
      } else if (now - _t0 >= kBootTimeoutMs) {
        _off_since = now;
        if (++_fails >= kBootRetries) {
          enter(AiPowerState::Fault, now);
          o.fault_raised = true;
        } else {
          enter(AiPowerState::Off, now);
        }
      } else if (!want) {
        _off_since = now;
        enter(AiPowerState::Off, now);
      }
      break;
    case AiPowerState::Running:
      if (!in.aux_present) {  // the supply is gone: nothing left to do but note it
        _off_since = now;
        enter(AiPowerState::Off, now);
      } else if (in.hung) {
        _restart = true;
        _reason = 4;
        enter(AiPowerState::Stopping, now);
        o.send_shutdown = true;
      } else if (!want) {
        _restart = false;
        _reason = why_not();
        enter(AiPowerState::Stopping, now);
        o.send_shutdown = true;
      }
      break;
    case AiPowerState::Stopping:
      // a hung module will not acknowledge: it gets the same time, then the buck is cut
      if (!in.alive_line || now - _t0 >= kStopTimeoutMs || !in.aux_present) {
        _off_since = now;
        enter(AiPowerState::Off, now);
        _restart = false;
      }
      break;
    case AiPowerState::Fault:
      if (now - _t0 >= kFaultRetryMs) {
        _fails = 0;
        _off_since = now;
        enter(AiPowerState::Off, now);
      }
      break;
  }

  o.buck_en = _s == AiPowerState::Booting || _s == AiPowerState::Running || _s == AiPowerState::Stopping;
  o.shutdown_req = _s == AiPowerState::Stopping;
  o.reason = _s == AiPowerState::Stopping ? _reason : 0;
  return o;
}

int16_t ai_ntc_temp_x10(uint32_t mv, uint32_t vref_mv, uint32_t r_fixed, uint32_t r0, uint32_t beta) {
  if (mv == 0 || mv >= vref_mv) return mv == 0 ? -400 : 1500;  // shorted / open: pretend cold or hot
  const double r = (double)r_fixed * mv / (double)(vref_mv - mv);
  const double t = 1.0 / (1.0 / 298.15 + log(r / r0) / beta) - 273.15;
  return (int16_t)lrint(t * 10.0);
}
