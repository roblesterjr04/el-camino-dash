#include "card_chan.h"

#include <math.h>
#include <string.h>

#include "card_pgm.h"
#include "elc_chan_msg.h"

namespace {
#define S(n, t) const char n[] PROGMEM = t;
S(N_SPEED, "Road speed") S(N_RPM, "Engine speed") S(N_FUEL, "Fuel level") S(N_TEMP, "Engine temp")
S(N_OIL, "Oil pressure") S(N_CHG, "Charging") S(N_AMB, "Ambient temp") S(N_ODO, "Odometer")
S(N_DTE, "Distance to empty") S(N_ECON, "Economy now") S(N_TRIP, "Economy trip") S(N_BATT, "Battery voltage")
#undef S

struct SensorDef {
  PGM_P name;
  uint8_t quantity;
  int8_t exponent;
  uint8_t flags;
  uint16_t period_ms;
  int32_t disp_min, disp_max;
};

// The center's "Core sensors" table (center_chan.cpp), plus 11. Sensors 0-10 carry no gauge / log flags: the center
// relays them on its own core sensors while it hears this node, so they would only be listed twice.
const SensorDef kSensor[CardChan::kSensors] PROGMEM = {
    {N_SPEED, ELC_Q_SPEED, -1, 0, 50, 0, 2000},          {N_RPM, ELC_Q_RPM, 0, 0, 50, 0, 6000},
    {N_FUEL, ELC_Q_PERCENT, -1, 0, 200, 0, 1000},        {N_TEMP, ELC_Q_TEMPERATURE, -1, 0, 200, 380, 1300},
    {N_OIL, ELC_Q_PRESSURE, 0, 0, 200, 0, 700},          {N_CHG, ELC_Q_SWITCH, 0, 0, 200, 0, 1},
    {N_AMB, ELC_Q_TEMPERATURE, -1, 0, 1000, -400, 600},  {N_ODO, ELC_Q_DISTANCE, 0, 0, 1000, 0, 0},
    {N_DTE, ELC_Q_DISTANCE, 0, 0, 1000, 0, 1000000},     {N_ECON, ELC_Q_ECONOMY, -2, 0, 1000, 0, 3000},
    {N_TRIP, ELC_Q_ECONOMY, -2, 0, 1000, 0, 3000},
    {N_BATT, ELC_Q_VOLTAGE, -2, ELC_SENSOR_F_LOG | ELC_SENSOR_F_GAUGE, 1000, 800, 1600},
};

const char kAlertText[] PROGMEM = "Calibrate temp/oil: Menu>Accessories>Sensor card";
const uint8_t kAlertGeneration = 1;

enum Stage : uint8_t { ST_IDLE = 0, ST_SENSORS, ST_ALERTS, ST_UI_ITEMS, ST_UI_VALUES };
const uint32_t kGapMs = 8;  // between description transfers
}  // namespace

void CardChan::begin(const Ctx &c, uint32_t now) {
  _c = c;
  for (uint8_t i = 0; i < kSensors; i++) _next[i] = now + 200u + (uint32_t)i * 17u;  // after the boot descriptions
  _alert_ms = now;
}

void CardChan::describe(uint8_t what) {
  _pending |= what & (ELC_NET_Q_SENSORS | ELC_NET_Q_ALERTS | ELC_NET_Q_UI);
}

bool CardChan::wantAlert() const {
  return _c.out->ign && (!cal::calibrated(*_c.cal, cal::TEMP) || !cal::calibrated(*_c.cal, cal::OIL));
}

void CardChan::sendAlertState(uint32_t now, bool force) {
  const bool on = wantAlert();
  if (!force && on == _alert_on && now - _alert_ms < 1000) return;  // 1 Hz, and at once on a change
  ElcAlertState st;
  st.active = on ? 1u : 0u;
  st.generation = kAlertGeneration;
  ElcChWriter w;
  elc_alert_encode(st, w);
  if (_c.net->send(ELC_CH_ALERT, w)) {
    _alert_on = on;
    _alert_ms = now;
  }
}

void CardChan::poll(uint32_t now) {
  if (!_c.net) return;
  // ---- descriptions, one transfer per pass at most, kGapMs apart
  if (_stage == ST_IDLE && _pending) {
    if (_pending & ELC_NET_Q_SENSORS) { _stage = ST_SENSORS; _pending &= (uint8_t)~ELC_NET_Q_SENSORS; }
    else if (_pending & ELC_NET_Q_ALERTS) { _stage = ST_ALERTS; _pending &= (uint8_t)~ELC_NET_Q_ALERTS; }
    else { _stage = ST_UI_ITEMS; _pending &= (uint8_t)~ELC_NET_Q_UI; }
    _idx = 0;
  }
  if (_stage != ST_IDLE && now - _last_send >= kGapMs) {
    bool sent = true, next_stage = false;
    ElcChWriter w;
    switch (_stage) {
      case ST_SENSORS: {
        SensorDef d;
        memcpy_P(&d, &kSensor[_idx], sizeof(d));
        ElcSensorDefine s;
        s.sensor = (uint8_t)_idx;
        s.quantity = d.quantity;
        s.exponent = d.exponent;
        s.flags = d.flags;
        s.period_ms = d.period_ms;
        s.disp_min = d.disp_min;
        s.disp_max = d.disp_max;
        strncpy_P(s.name, d.name, sizeof(s.name) - 1);
        elc_sensor_encode(s, w);
        sent = _c.net->send(ELC_CH_SENSOR, w);
        if (sent && ++_idx >= kSensors) next_stage = true;
        break;
      }
      case ST_ALERTS: {
        ElcAlertDefine a;
        a.alert = 0;
        a.severity = ELC_SEV_INFO;
        a.flags = 0;
        a.icon = 11;  // service (wrench)
        a.sound = 0xFFFF;
        strncpy_P(a.text, kAlertText, sizeof(a.text) - 1);
        elc_alert_encode(a, w);
        sent = _c.net->send(ELC_CH_ALERT, w);
        if (sent) {
          sendAlertState(now, true);
          next_stage = true;
        }
        break;
      }
      case ST_UI_ITEMS:
        if (!_c.ui->encodeItem((uint8_t)_idx, w)) {
          next_stage = true;
          break;
        }
        sent = _c.net->send(ELC_CH_UI, w);
        if (sent && ++_idx >= _c.ui->itemCount()) {
          _stage = ST_UI_VALUES;  // the settings' values follow, so the center can back them up
          _idx = 0;
        }
        break;
      case ST_UI_VALUES:
        while (_idx < _c.ui->itemCount() && !_c.ui->isSetting((uint8_t)_idx)) _idx++;
        if (_idx >= _c.ui->itemCount()) {
          next_stage = true;
          break;
        }
        _c.ui->encodeValue((uint8_t)_idx, w);
        sent = _c.net->send(ELC_CH_UI, w);
        if (sent) _idx++;
        break;
      default:
        next_stage = true;
        break;
    }
    if (sent) _last_send = now;
    if (next_stage) _stage = ST_IDLE;
  }
  sendAlertState(now, false);
  sendValues(now);
}

void CardChan::sendValues(uint32_t now) {
  const EngineOut &o = *_c.out;
  for (uint8_t i = 0; i < kSensors; i++) {
    if ((int32_t)(now - _next[i]) < 0) continue;
    int32_t v = 0;
    uint8_t st = ELC_SENSOR_OK;
    switch (i) {
      case 0: v = (int32_t)lroundf(o.mph * 16.09344f); break;
      case 1: v = (int32_t)o.engine_rpm; st = o.st_rpm; break;
      case 2: v = o.fuel_pct10; st = o.st_fuel; break;
      case 3: v = o.temp_c10; st = o.st_temp; break;
      case 4: v = o.oil_kpa; st = o.st_oil; break;
      case 5: v = o.charging ? 1 : 0; st = o.st_charge; break;
      case 6: v = o.amb_c10; st = o.st_amb; break;
      case 7: {
        const float m = (float)o.odo_tenths * 160.9344f;
        v = o.odo_tenths == kOdoUnknown ? 0 : (m > 2.0e9f ? 2000000000L : (int32_t)lroundf(m));
        st = o.st_odo;
        break;
      }
      case 8: v = (int32_t)lroundf((float)o.dte_tenths * 160.9344f); st = o.dte_tenths ? 0 : ELC_SENSOR_NOT_READY; break;
      case 9: v = (int32_t)lroundf((float)o.mpg_inst_x10 * 4.251437f); st = o.mpg_inst_x10 ? 0 : ELC_SENSOR_NOT_READY; break;
      case 10: v = (int32_t)lroundf((float)o.mpg_avg_x10 * 4.251437f); st = o.mpg_avg_x10 ? 0 : ELC_SENSOR_NOT_READY; break;
      default: v = o.batt_v100; break;
    }
    ElcSensorValue sv;
    sv.sensor = i;
    sv.value = v;
    sv.status = st;
    ElcChWriter w;
    elc_sensor_encode(sv, w);
    if (!_c.net->send(ELC_CH_SENSOR, w)) return;  // no room: try again next pass, in order
    const uint16_t period = pgm_read_word(&kSensor[i].period_ms);
    _next[i] += period;
    if ((int32_t)(now - _next[i]) > (int32_t)period) _next[i] = now + period;  // fell behind: do not burst
    _values_sent++;
  }
}
