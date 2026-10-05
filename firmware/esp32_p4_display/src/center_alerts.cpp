#include "center_alerts.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include <chan_screen.h>
#include <elc_chan_msg.h>
#include <sensor_link.h>

#include "center_ui.h"

namespace {

// Sensor-pack fault word (docs/protocols/sensor_uart.md, Fault word) and frame flags, as in center_chan.cpp.
constexpr uint16_t kPotOpen = 1 << 0, kPotShort = 1 << 1, kTempOpen = 1 << 2, kTempShort = 1 << 3, kPressOpen = 1 << 4,
                   kPressShort = 1 << 5, kOdoStore = 1 << 15;
constexpr uint8_t kFlagAltOk = 0x01, kFlagIgnOn = 0x02;

// Icons (can_channels.md, Registries).
constexpr uint8_t kIconTemp = 1, kIconOil = 2, kIconBattery = 3, kIconFuel = 4, kIconService = 11, kIconLink = 12;

// The center's own alerts, node 0x01 (can_channels.md, "Center alerts"). Numbers are stable: keep them.
enum Own : uint8_t {
  kDataLost = 0,
  kOilLow = 1,
  kHot = 2,
  kOverheat = 3,
  kNotCharging = 4,
  kFuelLow = 5,
  kOdoNotStored = 6,
  kSensorFault = 7,
  kNodeLost = 8,
};

struct OwnDef {
  uint8_t alert, severity, flags, icon;
  const char *text;
};
const OwnDef kOwn[] = {
    {kDataLost, ELC_SEV_ADVISORY, 0, kIconLink, "Sensor data lost"},
    {kOilLow, ELC_SEV_WARNING, ELC_ALERT_F_ACK, kIconOil, "Low oil pressure"},
    {kHot, ELC_SEV_CAUTION, 0, kIconTemp, "Engine hot"},
    {kOverheat, ELC_SEV_WARNING, ELC_ALERT_F_ACK, kIconTemp, "Engine overheating"},
    {kNotCharging, ELC_SEV_CAUTION, 0, kIconBattery, "Not charging"},
    {kFuelLow, ELC_SEV_ADVISORY, 0, kIconFuel, "Low fuel"},
    {kOdoNotStored, ELC_SEV_ADVISORY, 0, kIconService, "Odometer not stored"},
    {kSensorFault, ELC_SEV_ADVISORY, 0, kIconService, "Sensor fault"},
    {kNodeLost, ELC_SEV_ADVISORY, 0, kIconLink, "Module not responding"},
};

ElcAlertManager s_mgr;
bool s_begun = false;

void define(uint8_t alert, const char *text) {
  for (const OwnDef &o : kOwn) {
    if (o.alert != alert) continue;
    ElcAlertDefine d;
    d.alert = o.alert;
    d.severity = o.severity;
    d.flags = o.flags;
    d.icon = o.icon;
    d.sound = 0;  // the severity's default
    elc_ch_copy_text(d.text, sizeof(d.text), text ? text : o.text);
    s_mgr.defineOwn(d);
    return;
  }
}

/** A condition that must hold for hold_ms to raise, and its clear condition for clear_ms to clear (hysteresis). */
struct Debounce {
  bool on = false;
  uint32_t since = 0;
  bool pending = false;
  bool update(bool raise, bool clear, uint32_t now, uint32_t hold_ms, uint32_t clear_ms = 1000) {
    const bool want_change = on ? clear : raise;
    if (!want_change) {
      pending = false;
      return on;
    }
    if (!pending) {
      pending = true;
      since = now;
    }
    if (now - since >= (on ? clear_ms : hold_ms)) {
      on = !on;
      pending = false;
    }
    return on;
  }
};

Debounce s_oil, s_hot, s_overheat, s_charge, s_fuel, s_lost;
bool s_ever_fresh = false, s_ign_when_fresh = false;
uint32_t s_eval_ms = 0;

/** "Temp sender open", or nullptr when no sender faults. */
const char *fault_text(uint16_t f) {
  if (f & kTempOpen) return "Sensor fault: temp sender open";
  if (f & kTempShort) return "Sensor fault: temp sender short";
  if (f & kPressOpen) return "Sensor fault: oil sender open";
  if (f & kPressShort) return "Sensor fault: oil sender short";
  if (f & kPotOpen) return "Sensor fault: fuel sender open";
  if (f & kPotShort) return "Sensor fault: fuel sender short";
  return nullptr;
}

void evaluate(uint32_t now) {
  const SensorLink &link = center_sensor_link();
  const bool fresh = link.fresh();
  const SensorPacketValues &p = link.last();
  const SensorDerived s = link.derived();
  const uint16_t f = p.faults;
  const bool ign = (p.flags & kFlagIgnOn) != 0;
  const bool running = ign && p.engine_rpm > 400;

  if (fresh) {
    s_ever_fresh = true;
    s_ign_when_fresh = ign;
  }
  // Data lost only after data was seen with the key on (a key-off that stops the frames is not a fault).
  s_mgr.setOwn(kDataLost, s_lost.update(s_ever_fresh && s_ign_when_fresh && !fresh, fresh, now, 2000, 500));

  // With no fresh data the readings are not trusted: those alerts hold their state until data returns.
  if (fresh) {
    const bool oil_ok = !(f & (kPressOpen | kPressShort));
    const bool temp_ok = !(f & (kTempOpen | kTempShort));
    const bool fuel_ok = !(f & (kPotOpen | kPotShort));
    // Thresholds as the console's warnings (console_ui.cpp, build_alerts).
    s_mgr.setOwn(kOilLow, s_oil.update(oil_ok && running && s.oil_psi < 8.0f,
                                       !oil_ok || !running || s.oil_psi >= 10.0f, now, 2000));
    s_mgr.setOwn(kHot, s_hot.update(temp_ok && s.coolant_temp_f > 230.0f,
                                    !temp_ok || s.coolant_temp_f < 225.0f, now, 3000, 3000));
    s_mgr.setOwn(kOverheat, s_overheat.update(temp_ok && s.coolant_temp_f > 245.0f,
                                              !temp_ok || s.coolant_temp_f < 240.0f, now, 3000, 3000));
    s_mgr.setOwn(kNotCharging, s_charge.update(running && !(p.flags & kFlagAltOk),
                                               !running || (p.flags & kFlagAltOk), now, 5000, 2000));
    s_mgr.setOwn(kFuelLow, s_fuel.update(fuel_ok && s.fuel_pct < 12.0f, !fuel_ok || s.fuel_pct > 15.0f, now,
                                         10000, 10000));  // slosh
    s_mgr.setOwn(kOdoNotStored, (f & kOdoStore) != 0);
    const char *ft = fault_text(f);
    if (ft) define(kSensorFault, ft);
    s_mgr.setOwn(kSensorFault, ft != nullptr);
  }

  // A node whose alerts went silent: one advisory, named when it is one node.
  const uint32_t lost = s_mgr.lostNodes();
  if (lost) {
    char text[49];
    int n = 0;
    uint8_t first = 0;
    for (uint8_t i = 0; i < 32; i++) {
      if (!((lost >> i) & 1u)) continue;
      if (!n++) first = i;
    }
    ElcChannels *ch = chan_screen();
    const char *name = ch && ch->nodeInfo(first).announced ? ch->nodeInfo(first).a.name : "";
    if (n == 1 && name[0]) {
      snprintf(text, sizeof(text), "%s not responding", name);
    } else if (n == 1) {
      snprintf(text, sizeof(text), "Module 0x%02X not responding", (unsigned)first);
    } else {
      snprintf(text, sizeof(text), "%d modules not responding", n);
    }
    define(kNodeLost, text);
  }
  s_mgr.setOwn(kNodeLost, lost != 0);
}

void on_transfer(const ElcChTransfer &t, void *) { s_mgr.feed(t, millis()); }

}  // namespace

void center_alerts_begin() {
  ElcChannels *ch = chan_screen();
  if (!ch || s_begun) return;
  s_mgr.begin(ch);
  for (const OwnDef &o : kOwn) define(o.alert, nullptr);
  chan_screen_on_transfer(on_transfer, nullptr);
  s_begun = true;
}

void center_alerts_loop(uint32_t now) {
  if (!s_begun) {
    center_alerts_begin();
    if (!s_begun) return;
  }
  if (now - s_eval_ms >= 100u) {
    s_eval_ms = now;
    evaluate(now);
  }
  s_mgr.poll(now);
}

void center_alerts_describe(uint8_t what) {
  if (what & ELC_NET_Q_ALERTS) s_mgr.describeOwn();
}

ElcAlertManager &center_alerts() { return s_mgr; }

bool center_alerts_command(const char *line, char *reply, size_t n) {
  if (strncmp(line, "alerts", 6) != 0 || (line[6] && line[6] != ' ')) return false;
  if (!s_begun) {
    snprintf(reply, n, "alerts: not started");
    return true;
  }
  const uint32_t now = millis();
  if (strcmp(line, "alerts ack all") == 0) {
    snprintf(reply, n, "acknowledged %d", s_mgr.ackAll(now));
  } else if (strcmp(line, "alerts ack") == 0) {
    const ElcAlertManager::Alert *t = s_mgr.top();
    if (t) {
      char text[49];
      snprintf(text, sizeof(text), "%s", t->text);
      const uint8_t node = t->node, alert = t->alert;
      snprintf(reply, n, s_mgr.ack(node, alert, now) ? "acknowledged 0x%02X #%u %s" : "could not acknowledge 0x%02X #%u %s",
               (unsigned)node, (unsigned)alert, text);
    } else {
      snprintf(reply, n, "nothing to acknowledge");
    }
  } else if (strcmp(line, "alerts") == 0) {
    s_mgr.describe(reply, n);
  } else {
    snprintf(reply, n, "usage: alerts | alerts ack | alerts ack all");
  }
  return true;
}
