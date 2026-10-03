#include "center_chan.h"

#include <Arduino.h>
#include <sys/time.h>
#include <time.h>

#include <chan_screen.h>
#include <cluster_i2c.h>
#include <display_setup.h>
#include <el_camino_speed.h>
#include <elc_chan_msg.h>
#include <sensor_link.h>

#include "center_camera.h"
#include "center_menu.h"
#include "center_store.h"
#include "center_time.h"
#include "center_ui.h"

#ifndef WHEEL_CIRCUMFERENCE_M
#define WHEEL_CIRCUMFERENCE_M 2.1f
#endif

namespace {

// Sensor-pack fault word (docs/protocols/sensor_uart.md, Fault word) and frame flags (gauge_link.cpp).
constexpr uint16_t kPotOpen = 1 << 0, kPotShort = 1 << 1, kTempOpen = 1 << 2, kTempShort = 1 << 3, kPressOpen = 1 << 4,
                   kPressShort = 1 << 5, kCoilMissing = 1 << 9, kAltStuck = 1 << 10, kOdoStore = 1 << 15;
constexpr uint8_t kFlagAltOk = 0x01, kFlagIgnOn = 0x02;

struct Core {
  const char *name;
  uint8_t quantity;
  int8_t exponent;
  uint16_t period_ms;
  int32_t disp_min, disp_max;
};
// The "Core sensors" table: sensor number = index.
const Core kCore[] = {
    {"Road speed", ELC_Q_SPEED, -1, 50, 0, 2000},
    {"Engine speed", ELC_Q_RPM, 0, 50, 0, 6000},
    {"Fuel level", ELC_Q_PERCENT, -1, 200, 0, 1000},
    {"Engine temp", ELC_Q_TEMPERATURE, -1, 200, 380, 1300},
    {"Oil pressure", ELC_Q_PRESSURE, 0, 200, 0, 700},
    {"Charging", ELC_Q_SWITCH, 0, 200, 0, 1},
    {"Ambient temp", ELC_Q_TEMPERATURE, -1, 1000, -400, 600},
    {"Odometer", ELC_Q_DISTANCE, 0, 1000, 0, 0},
    {"Distance to empty", ELC_Q_DISTANCE, 0, 1000, 0, 1000000},
    {"Economy now", ELC_Q_ECONOMY, -2, 1000, 0, 3000},
    {"Economy trip", ELC_Q_ECONOMY, -2, 1000, 0, 3000},
};
constexpr int kCoreN = sizeof(kCore) / sizeof(kCore[0]);
uint32_t s_next[kCoreN] = {0};
uint32_t s_time_ms = 0, s_vehicle_ms = 0, s_prefs_ms = 0;
ElcSysPrefs s_prefs_sent;
bool s_prefs_valid = false;

void send_defines() {
  ElcChannels *ch = chan_screen();
  if (!ch) return;
  for (int i = 0; i < kCoreN; i++) {
    ElcSensorDefine d;
    d.sensor = (uint8_t)i;
    d.quantity = kCore[i].quantity;
    d.exponent = kCore[i].exponent;
    d.flags = ELC_SENSOR_F_LOG | ELC_SENSOR_F_GAUGE;  // bit2 clear: the center words its own warnings
    d.period_ms = kCore[i].period_ms;
    d.disp_min = kCore[i].disp_min;
    d.disp_max = kCore[i].disp_max;
    elc_ch_copy_text(d.name, sizeof(d.name), kCore[i].name);
    ElcChWriter w;
    elc_sensor_encode(d, w);
    ch->send(ELC_CH_SENSOR, w);
  }
}

void on_transfer(const ElcChTransfer &t, void *) { center_store_transfer(t); }

void on_describe(uint8_t what, void *) {
  if (what & ELC_NET_Q_SENSORS) send_defines();
}

int32_t r(float v) { return (int32_t)lroundf(v); }

/** One core reading: value in its exponent's units and a status. */
void reading(int i, const SensorLink &link, int32_t *value, uint8_t *status) {
  const SensorPacketValues &p = link.last();
  const SensorDerived s = link.derived();
  const bool fresh = link.fresh();
  const uint16_t f = p.faults;
  *value = 0;
  *status = fresh ? ELC_SENSOR_OK : ELC_SENSOR_NOT_READY;  // no data for 400 ms: every core sensor is not ready
  if (!fresh) return;
  auto open_short = [&](uint16_t open, uint16_t shrt) {
    if (f & open) *status = ELC_SENSOR_OPEN;
    else if (f & shrt) *status = ELC_SENSOR_SHORT;
  };
  switch (i) {
    case 0: *value = r(elc_speed_rpm_to_mph(p.speed_rpm, WHEEL_CIRCUMFERENCE_M, 1.0f) * 1.609344f * 10.0f); break;
    case 1:
      *value = (int32_t)p.engine_rpm;
      if (f & kCoilMissing) *status = ELC_SENSOR_NOT_READY;
      break;
    case 2: *value = r(s.fuel_pct * 10.0f); open_short(kPotOpen, kPotShort); break;
    case 3: *value = r((s.coolant_temp_f - 32.0f) * 50.0f / 9.0f); open_short(kTempOpen, kTempShort); break;
    case 4: *value = r(s.oil_psi * 6.894757f); open_short(kPressOpen, kPressShort); break;
    case 5:
      *value = (p.flags & kFlagIgnOn) && (p.flags & kFlagAltOk) ? 1 : 0;
      if (f & kAltStuck) *status = ELC_SENSOR_FAULT;
      break;
    case 6: {  // the ambient sender is the same thermistor type as the engine's
      SensorPacketValues q = p;
      q.therm_adc = p.ambient_adc;
      *value = r((SensorPacketParser::derive(q).coolant_temp_f - 32.0f) * 50.0f / 9.0f);
      break;
    }
    case 7: {
      float mi = 0;
      if (f & kOdoStore) *status = ELC_SENSOR_FAULT;
      else if (!link.odometerMiles(&mi)) *status = ELC_SENSOR_NOT_READY;
      *value = r(mi * 1609.344f);
      break;
    }
    case 8:
      if (!p.has_fuel_economy) *status = ELC_SENSOR_NOT_READY;
      *value = r((float)p.dte_tenths_mile * 160.9344f);
      break;
    case 9:
    case 10:
      if (!p.has_fuel_economy) *status = ELC_SENSOR_NOT_READY;
      *value = r((float)(i == 9 ? p.mpg_instant_x10 : p.mpg_average_x10) * 0.1f * 0.4251437f * 100.0f);
      break;
  }
}

void send_time(ElcChannels *ch) {
  uint64_t ms = 0;
  if (!center_wall_epoch_ms(&ms)) return;
  const time_t t = (time_t)(ms / 1000u);
  struct tm lt, gt;
  localtime_r(&t, &lt);
  gmtime_r(&t, &gt);
  // newlib has no tm_gmtoff: local minus UTC, across a date change
  int days = lt.tm_year != gt.tm_year ? (lt.tm_year > gt.tm_year ? 1 : -1) : lt.tm_yday - gt.tm_yday;
  if (days > 1) days = -1;  // year wrap the other way
  if (days < -1) days = 1;
  const long off = days * 86400L + (lt.tm_hour - gt.tm_hour) * 3600L + (lt.tm_min - gt.tm_min) * 60L;
  ElcSysTime v;
  v.utc = (uint32_t)t;
  v.offset_q = (int8_t)(off / 900);
  v.source = ELC_TIME_SRC_GPS;
  ElcChWriter w;
  elc_sys_encode(v, w);
  ch->send(ELC_CH_SYSTEM, w);
}

void send_vehicle(ElcChannels *ch, const SensorLink &link) {
  const SensorPacketValues &p = link.last();
  const bool fresh = link.fresh();
  const float mph = fresh ? elc_speed_rpm_to_mph(p.speed_rpm, WHEEL_CIRCUMFERENCE_M, 1.0f) : 0.0f;
  ElcSysVehicle v;
  if (fresh && (p.flags & kFlagIgnOn)) v.flags |= ELC_VEH_IGNITION;
  if (fresh && p.engine_rpm > 300) v.flags |= ELC_VEH_RUNNING;
  if (elc_illumination_is_night()) v.flags |= ELC_VEH_HEADLIGHTS | ELC_VEH_NIGHT;
  if (center_camera_reverse()) v.flags |= ELC_VEH_REVERSE;
  if (mph > 1.0f) v.flags |= ELC_VEH_MOVING;
  if (fresh) v.flags |= ELC_VEH_SENSORS_LIVE;
  v.backlight = center_menu_brightness();
  v.speed_x10 = fresh ? (uint16_t)lroundf(mph * 16.09344f) : 0xFFFF;
  ElcChWriter w;
  elc_sys_encode(v, w);
  ch->send(ELC_CH_SYSTEM, w);
}

void send_prefs(ElcChannels *ch, bool force) {
  ElcSysPrefs v;
  v.units = 0;  // US: miles, °F, psi (the cluster has no unit setting yet)
  v.theme = (uint8_t)center_ui_cluster_theme();
  v.clock24 = 0;
  if (!force && s_prefs_valid && v.units == s_prefs_sent.units && v.theme == s_prefs_sent.theme &&
      v.clock24 == s_prefs_sent.clock24) {
    return;
  }
  s_prefs_sent = v;
  s_prefs_valid = true;
  ElcChWriter w;
  elc_sys_encode(v, w);
  ch->send(ELC_CH_SYSTEM, w);
}

}  // namespace

void center_chan_begin() {
  chan_screen_on_describe(on_describe, nullptr);
  chan_screen_on_transfer(on_transfer, nullptr);
}

void center_chan_loop(uint32_t now) {
  ElcChannels *ch = chan_screen();
  if (!ch) return;
  const SensorLink &link = center_sensor_link();
  for (int i = 0; i < kCoreN; i++) {
    if ((int32_t)(now - s_next[i]) < 0) continue;
    s_next[i] = now + kCore[i].period_ms;
    ElcSensorValue v;
    v.sensor = (uint8_t)i;
    reading(i, link, &v.value, &v.status);
    ElcChWriter w;
    elc_sensor_encode(v, w);
    ch->send(ELC_CH_SENSOR, w);
  }
  if (now - s_time_ms >= 10000u || !s_time_ms) {
    s_time_ms = now ? now : 1;
    send_time(ch);
  }
  if (now - s_vehicle_ms >= 200u) {
    s_vehicle_ms = now;
    send_vehicle(ch, link);
  }
  const bool periodic = now - s_prefs_ms >= 5000u;
  if (periodic) s_prefs_ms = now;
  send_prefs(ch, periodic);
}
