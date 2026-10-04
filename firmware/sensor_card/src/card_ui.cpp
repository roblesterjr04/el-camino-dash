#include "card_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "card_pgm.h"
#include "elc_chan_msg.h"

// ---- the menu table (flash on the AVR)

enum Unit : uint8_t { U_NONE, U_VOL, U_TEMP, U_DTEMP, U_PRESS, U_VOLT, U_PCT, U_CTS, U_MI, U_PPM };

struct ItemDef {
  uint8_t id, parent, kind, flags, unit;
  PGM_P label;
  PGM_P options;
};

namespace {
#define S(n, t) const char n[] PROGMEM = t;
S(L_FUEL, "Fuel") S(L_ETEMP, "Engine temp") S(L_OILP, "Oil pressure") S(L_AMB, "Ambient temp") S(L_BATT, "Battery")
S(L_SPDTACH, "Speed & tach") S(L_SERVICE, "Service") S(L_FUEL_LEVEL, "Fuel level") S(L_FUEL_RAW, "Fuel raw")
S(L_TANK, "Tank capacity") S(L_BIAS, "Bias resistor") S(L_WIRING, "Wiring") S(L_SENDER, "Sender")
S(L_FUEL_NOW, "Fuel in tank now") S(L_TABLE, "Table") S(L_CLEAR, "Clear table") S(L_TEMP, "Temperature")
S(L_TEMP_RAW, "Temp raw") S(L_OFFSET, "Offset") S(L_COOLANT_NOW, "Coolant is now") S(L_OIL_RAW, "Oil raw")
S(L_OIL_NOW, "Oil pressure is now") S(L_AMBIENT, "Ambient") S(L_TRIM, "Trim") S(L_BATT_NOW, "Battery is now")
S(L_PPM, "Speed pulses per mile") S(L_GPSCAL, "Calibrate with GPS") S(L_SPDSTAT, "Speed status")
S(L_PPR, "Tach pulses per rev") S(L_CALSTAT, "Calibration status") S(L_RESET, "Reset calibration")
S(L_SETODO, "Set odometer") S(L_CARDID, "Card ID") S(L_SENSOR, "Sensor")
S(L_P1, "Point 1") S(L_P2, "Point 2") S(L_P3, "Point 3") S(L_P4, "Point 4") S(L_P5, "Point 5") S(L_P6, "Point 6")
S(L_P7, "Point 7") S(L_P8, "Point 8")

S(O_FUEL_BIAS, "100 ohm (JP6)\n220 ohm (JP7)\n470 ohm (JP8)\n69 ohm (JP6+7)\n83 ohm (JP6+8)\n150 ohm (JP7+8)\n60 ohm (all)")
S(O_TEMP_BIAS, "220 ohm (JP9)\n1k (JP10)\n10k (JP11)\n180 ohm (JP9+10)\n215 ohm (JP9+11)\n909 ohm (JP10+11)")
S(O_OIL_BIAS, "220 ohm (JP12)\n1k (JP13)\n100k (JP14)\n180 ohm (JP12+13)\n990 ohm (JP13+14)")
S(O_WIRE_F, "Pull-up (JP2 1-2)\nPull-down (JP2 2-3)") S(O_WIRE_T, "Pull-up (JP3 1-2)\nPull-down (JP3 2-3)")
S(O_WIRE_O, "Pull-up (JP4 1-2)\nPull-down (JP4 2-3)")
S(O_FUEL_SENDER, "None (points only)\nGM 0-30 ohm (to 1964)\nGM 0-90 ohm (1965-97)\nFord 73-10 ohm\n240-33 ohm aftermarket\nVDO 10-180 ohm")
S(O_TEMP_SENDER, "None (points only)\n100k NTC B3950\n10k NTC B3950\nGM 2-wire coolant")
S(O_AMB, "100k NTC (JP16)\n10k NTC (JP15)") S(O_PPR, "From J5 jumpers\n2\n3\n4\n6\n8")
#undef S

// bias ohms x10 of each option (the parallel values are the exact ones: 100||220 = 68.8)
const uint32_t B_FUEL[] PROGMEM = {1000, 2200, 4700, 688, 825, 1495, 594};
const uint32_t B_TEMP[] PROGMEM = {2200, 10000, 100000, 1803, 2153, 9091};
const uint32_t B_OIL[] PROGMEM = {2200, 10000, 1000000, 1803, 9901};
const uint8_t kPpr[] = {0, 2, 3, 4, 6, 8};

const uint8_t RO = ELC_UI_F_RO, CONF = ELC_UI_F_CONFIRM, ADV = ELC_UI_F_ADVANCED, LIVE = ELC_UI_F_LIVE,
              SET = ELC_UI_F_SETTING;
const uint8_t GRP = ELC_UI_GROUP, RDO = ELC_UI_READOUT, NUM = ELC_UI_NUMBER, CHO = ELC_UI_CHOICE,
              ACT = ELC_UI_ACTION, TXT = ELC_UI_TEXTOUT;
}  // namespace

static const ItemDef kItems[] PROGMEM = {
    {0, 0xFF, GRP, 0, U_NONE, L_FUEL, nullptr}, {1, 0xFF, GRP, 0, U_NONE, L_ETEMP, nullptr},
    {2, 0xFF, GRP, 0, U_NONE, L_OILP, nullptr}, {3, 0xFF, GRP, 0, U_NONE, L_AMB, nullptr},
    {4, 0xFF, GRP, 0, U_NONE, L_BATT, nullptr}, {5, 0xFF, GRP, 0, U_NONE, L_SPDTACH, nullptr},
    {6, 0xFF, GRP, 0, U_NONE, L_SERVICE, nullptr},
    // fuel
    {10, 0, RDO, RO | LIVE, U_VOL, L_FUEL_LEVEL, nullptr}, {11, 0, RDO, RO | LIVE | ADV, U_CTS, L_FUEL_RAW, nullptr},
    {12, 0, NUM, SET, U_VOL, L_TANK, nullptr}, {13, 0, CHO, SET, U_NONE, L_BIAS, O_FUEL_BIAS},
    {14, 0, CHO, SET, U_NONE, L_WIRING, O_WIRE_F}, {15, 0, CHO, SET, U_NONE, L_SENDER, O_FUEL_SENDER},
    {16, 0, NUM, CONF, U_VOL, L_FUEL_NOW, nullptr}, {17, 0, TXT, RO, U_NONE, L_TABLE, nullptr},
    {18, 0, ACT, CONF, U_NONE, L_CLEAR, nullptr},
    {20, 0, NUM, SET | ADV, U_NONE, L_P1, nullptr}, {21, 0, NUM, SET | ADV, U_NONE, L_P2, nullptr},
    {22, 0, NUM, SET | ADV, U_NONE, L_P3, nullptr}, {23, 0, NUM, SET | ADV, U_NONE, L_P4, nullptr},
    {24, 0, NUM, SET | ADV, U_NONE, L_P5, nullptr}, {25, 0, NUM, SET | ADV, U_NONE, L_P6, nullptr},
    {26, 0, NUM, SET | ADV, U_NONE, L_P7, nullptr}, {27, 0, NUM, SET | ADV, U_NONE, L_P8, nullptr},
    // engine temperature
    {30, 1, RDO, RO | LIVE, U_TEMP, L_TEMP, nullptr}, {31, 1, RDO, RO | LIVE | ADV, U_CTS, L_TEMP_RAW, nullptr},
    {32, 1, CHO, SET, U_NONE, L_BIAS, O_TEMP_BIAS}, {33, 1, CHO, SET, U_NONE, L_WIRING, O_WIRE_T},
    {34, 1, CHO, SET, U_NONE, L_SENDER, O_TEMP_SENDER}, {35, 1, NUM, SET, U_DTEMP, L_OFFSET, nullptr},
    {36, 1, NUM, CONF, U_TEMP, L_COOLANT_NOW, nullptr}, {37, 1, TXT, RO, U_NONE, L_TABLE, nullptr},
    {38, 1, ACT, CONF, U_NONE, L_CLEAR, nullptr},
    {40, 1, NUM, SET | ADV, U_NONE, L_P1, nullptr}, {41, 1, NUM, SET | ADV, U_NONE, L_P2, nullptr},
    {42, 1, NUM, SET | ADV, U_NONE, L_P3, nullptr}, {43, 1, NUM, SET | ADV, U_NONE, L_P4, nullptr},
    {44, 1, NUM, SET | ADV, U_NONE, L_P5, nullptr}, {45, 1, NUM, SET | ADV, U_NONE, L_P6, nullptr},
    // oil pressure
    {50, 2, RDO, RO | LIVE, U_PRESS, L_OILP, nullptr}, {51, 2, RDO, RO | LIVE | ADV, U_CTS, L_OIL_RAW, nullptr},
    {52, 2, CHO, SET, U_NONE, L_BIAS, O_OIL_BIAS}, {53, 2, CHO, SET, U_NONE, L_WIRING, O_WIRE_O},
    {54, 2, NUM, CONF, U_PRESS, L_OIL_NOW, nullptr}, {55, 2, TXT, RO, U_NONE, L_TABLE, nullptr},
    {56, 2, ACT, CONF, U_NONE, L_CLEAR, nullptr},
    {60, 2, NUM, SET | ADV, U_NONE, L_P1, nullptr}, {61, 2, NUM, SET | ADV, U_NONE, L_P2, nullptr},
    {62, 2, NUM, SET | ADV, U_NONE, L_P3, nullptr}, {63, 2, NUM, SET | ADV, U_NONE, L_P4, nullptr},
    // ambient
    {70, 3, RDO, RO | LIVE, U_TEMP, L_AMBIENT, nullptr}, {72, 3, CHO, SET, U_NONE, L_SENSOR, O_AMB},
    {73, 3, NUM, SET, U_DTEMP, L_OFFSET, nullptr},
    // battery
    {80, 4, RDO, RO | LIVE, U_VOLT, L_BATT, nullptr}, {81, 4, NUM, SET | ADV, U_PCT, L_TRIM, nullptr},
    {82, 4, NUM, CONF, U_VOLT, L_BATT_NOW, nullptr},
    // speed and tach
    {90, 5, NUM, SET, U_PPM, L_PPM, nullptr}, {91, 5, ACT, CONF, U_NONE, L_GPSCAL, nullptr},
    {92, 5, TXT, RO | LIVE, U_NONE, L_SPDSTAT, nullptr}, {93, 5, CHO, SET, U_NONE, L_PPR, O_PPR},
    // service
    {100, 6, TXT, RO, U_NONE, L_CALSTAT, nullptr}, {101, 6, ACT, CONF, U_NONE, L_RESET, nullptr},
    {102, 6, NUM, CONF | ADV, U_MI, L_SETODO, nullptr}, {103, 6, TXT, RO | ADV, U_NONE, L_CARDID, nullptr},
};
static const uint8_t kItemCount = sizeof(kItems) / sizeof(kItems[0]);

static ItemDef read_def(uint8_t index) {
  ItemDef d;
  memcpy_P(&d, &kItems[index], sizeof(d));
  return d;
}

// ---- display units

static const float kLitresPerGal = 3.785411784f;
static const float kPsiPerKpa = 1.0f / 6.894757f;

int32_t CardUi::dispVolume(int32_t l10) const { return (_units & 1) ? l10 : (int32_t)lroundf((float)l10 / kLitresPerGal); }
int32_t CardUi::fromVolume(int32_t d) const { return (_units & 1) ? d : (int32_t)lroundf((float)d * kLitresPerGal); }
int32_t CardUi::dispTemp(int32_t c10) const { return (_units & 2) ? c10 : (int32_t)lroundf((float)c10 * 1.8f + 320.0f); }
int32_t CardUi::fromTemp(int32_t d) const { return (_units & 2) ? d : (int32_t)lroundf(((float)d - 320.0f) / 1.8f); }
int32_t CardUi::dispDelta(int32_t c10) const { return (_units & 2) ? c10 : (int32_t)lroundf((float)c10 * 1.8f); }
int32_t CardUi::fromDelta(int32_t d) const { return (_units & 2) ? d : (int32_t)lroundf((float)d / 1.8f); }
int32_t CardUi::dispPress(int32_t kpa) const { return (_units & 4) ? kpa : (int32_t)lroundf((float)kpa * kPsiPerKpa * 10.0f); }
int32_t CardUi::fromPress(int32_t d) const { return (_units & 4) ? d : (int32_t)lroundf((float)d / 10.0f / kPsiPerKpa); }

static const char *unit_text(uint8_t unit, uint8_t units) {
  switch (unit) {
    case U_VOL: return (units & 1) ? "L" : "gal";
    case U_TEMP:
    case U_DTEMP: return (units & 2) ? "\xC2\xB0" "C" : "\xC2\xB0" "F";
    case U_PRESS: return (units & 4) ? "kPa" : "psi";
    case U_VOLT: return "V";
    case U_PCT: return "%";
    case U_CTS: return "cts";
    case U_MI: return "mi";
    case U_PPM: return "p/mi";
    default: return "";
  }
}

static int8_t unit_exp(uint8_t unit, uint8_t units) {
  switch (unit) {
    case U_VOL: case U_TEMP: case U_DTEMP: case U_MI: return -1;
    case U_PRESS: return (units & 4) ? 0 : -1;
    case U_VOLT: case U_PCT: return -2;
    default: return 0;
  }
}

void CardUi::setUnits(uint8_t units) {
  units &= 7;
  if (units == _units) return;
  _units = units;
  if (_c.net) _c.net->descriptionsChanged();  // units, ranges and exponents changed: the center asks again
}

// ---- table lookups

uint8_t CardUi::itemCount() const { return kItemCount; }

const ItemDef *CardUi::def(uint8_t id) const {
  for (uint8_t i = 0; i < kItemCount; i++) {
    if (pgm_read_byte(&kItems[i].id) == id) return &kItems[i];
  }
  return nullptr;
}

static int index_of(uint8_t id) {
  for (uint8_t i = 0; i < kItemCount; i++) {
    if (pgm_read_byte(&kItems[i].id) == id) return i;
  }
  return -1;
}

static uint8_t channel_of(uint8_t id) {  // fuel 0, temp 1, oil 2 for the items of those groups
  if (id >= 10 && id < 30) return cal::FUEL;
  if (id >= 30 && id < 50) return cal::TEMP;
  return cal::OIL;
}

static const uint32_t *bias_table(uint8_t ch, uint8_t *n) {
  if (ch == cal::FUEL) { *n = sizeof(B_FUEL) / 4; return B_FUEL; }
  if (ch == cal::TEMP) { *n = sizeof(B_TEMP) / 4; return B_TEMP; }
  *n = sizeof(B_OIL) / 4;
  return B_OIL;
}

static uint8_t bias_index(uint8_t ch, uint32_t bias_x10) {
  uint8_t n;
  const uint32_t *t = bias_table(ch, &n);
  uint8_t best = 0;
  float bd = 1e9f;
  for (uint8_t i = 0; i < n; i++) {
    const float o = (float)pgm_read_dword(&t[i]);
    const float d = fabsf(logf((float)bias_x10 / o));
    if (d < bd) {
      bd = d;
      best = i;
    }
  }
  return best;
}

void CardUi::rangeOf(uint8_t id, int8_t *exp, int32_t *mn, int32_t *mx, int32_t *step) const {
  const ItemDef *d = def(id);
  const uint8_t unit = d ? pgm_read_byte(&d->unit) : (uint8_t)U_NONE;
  *exp = unit_exp(unit, _units);
  *mn = 0;
  *mx = 0;
  *step = 1;
  if (!d) return;
  switch (id) {
    case 12: *mn = dispVolume(189); *mx = dispVolume(1514); break;  // 5.0 - 40.0 gal
    case 16: *mn = 0; *mx = dispVolume(1600); break;
    case 35: case 73: *mn = -100; *mx = 100; break;
    case 36: *mn = dispTemp(-400); *mx = dispTemp(2000); break;
    case 54: *mn = 0; *mx = dispPress(2500); break;
    case 81: *mn = -500; *mx = 500; break;
    case 82: *mn = 800; *mx = 2000; break;
    case 90: *mn = 500; *mx = 50000; break;
    case 102: *mn = 0; *mx = 9999999; break;
    default:
      if ((id >= 20 && id <= 27) || (id >= 40 && id <= 45) || (id >= 60 && id <= 63)) {
        *mn = -1;
        *mx = 2147483647L;
      }
      break;
  }
}

bool CardUi::encodeItem(uint8_t index, ElcChWriter &w) {
  if (index >= kItemCount) return false;
  const ItemDef d = read_def(index);
  ElcUiItem it;
  it.item = d.id;
  it.parent = d.parent;
  it.kind = d.kind;
  it.flags = d.flags;
  rangeOf(d.id, &it.exponent, &it.min, &it.max, &it.step);
  strncpy_P(it.label, d.label, sizeof(it.label) - 1);
  it.label[sizeof(it.label) - 1] = 0;
  if (d.unit != U_NONE) elc_ch_copy_text(it.unit, sizeof(it.unit), unit_text(d.unit, _units));
  if (d.options) {
    strncpy_P(it.options, d.options, sizeof(it.options) - 1);
    it.options[sizeof(it.options) - 1] = 0;
  }
  elc_ui_encode(it, w);
  return w.ok();
}

bool CardUi::isSetting(uint8_t index) const {
  return index < kItemCount && (pgm_read_byte(&kItems[index].flags) & ELC_UI_F_SETTING);
}

// ---- values

static int32_t pack_point(const cal::ChannelCal &k, uint8_t slot) {
  if (slot >= k.n) return -1;
  return (int32_t)(((uint32_t)k.pts[slot].raw << 16) | (uint16_t)k.pts[slot].value);
}

int32_t CardUi::value(uint8_t id) const {
  const cal::Calibration &c = *_c.cal;
  const EngineOut &o = *_c.out;
  switch (id) {
    case 10: case 16: return o.st_fuel == cal::ST_OK ? dispVolume(o.fuel_l10) : 0;
    case 11: return (int32_t)lroundf(_c.engine->lastRaw(0));
    case 12: return dispVolume(c.tank_x10);
    case 13: return bias_index(cal::FUEL, c.fuel.bias_x10);
    case 14: return c.fuel.topology;
    case 15: return c.fuel.preset;
    case 30: case 36: return o.st_temp == cal::ST_OK ? dispTemp(o.temp_c10) : 0;
    case 31: return (int32_t)lroundf(_c.engine->lastRaw(1));
    case 32: return bias_index(cal::TEMP, c.temp.bias_x10);
    case 33: return c.temp.topology;
    case 34: return c.temp.preset;
    case 35: return dispDelta(c.temp.offset);
    case 50: case 54: return o.st_oil == cal::ST_OK ? dispPress(o.oil_kpa) : 0;
    case 51: return (int32_t)lroundf(_c.engine->lastRaw(2));
    case 52: return bias_index(cal::OIL, c.oil.bias_x10);
    case 53: return c.oil.topology;
    case 70: return o.st_amb == cal::ST_OK ? dispTemp(o.amb_c10) : 0;
    case 72: return c.amb_ntc;
    case 73: return dispDelta(c.amb_offset);
    case 80: case 82: return o.batt_v100;
    case 81: return c.batt_trim_ppm / 100;
    case 90: return (int32_t)(c.speed_ppm_x100 / 100);
    case 93: {
      for (uint8_t i = 0; i < sizeof(kPpr); i++) if (kPpr[i] == c.tach_ppr) return i;
      return 0;
    }
    case 102: return _c.odo->known() ? (int32_t)_c.odo->tenths() : 0;
    default: break;
  }
  if (id >= 20 && id <= 27) return pack_point(c.fuel, (uint8_t)(id - 20));
  if (id >= 40 && id <= 45) return pack_point(c.temp, (uint8_t)(id - 40));
  if (id >= 60 && id <= 63) return pack_point(c.oil, (uint8_t)(id - 60));
  return 0;
}

void CardUi::tableText(uint8_t ch, char *out, size_t cap) const {
  const cal::ChannelCal &k = cal::channel(*_c.cal, ch);
  if (_table_note[ch][0]) {
    snprintf(out, cap, "%s", _table_note[ch]);
    return;
  }
  if (!k.n) {
    snprintf(out, cap, "No points");
    return;
  }
  const uint8_t unit = ch == cal::FUEL ? U_VOL : ch == cal::TEMP ? U_TEMP : U_PRESS;
  const int8_t e = unit_exp(unit, _units);
  size_t n = 0;
  out[0] = 0;
  for (uint8_t i = 0; i < k.n && n + 12 < cap; i++) {
    const int32_t v = ch == cal::FUEL ? dispVolume(k.pts[i].value) : ch == cal::TEMP ? dispTemp(k.pts[i].value)
                                                                                      : dispPress(k.pts[i].value);
    char num[12];
    elc_ui_format(v, e, num, sizeof(num));
    n += (size_t)snprintf(out + n, cap - n, "%s%u>%s", i ? " " : "", (unsigned)k.pts[i].raw, num);
  }
}

void CardUi::speedText(char *out, size_t cap) const {
  if (_speed_note[0]) {
    snprintf(out, cap, "%s", _speed_note);
    return;
  }
  const cal::Calibration &c = *_c.cal;
  const unsigned long ppm = c.speed_ppm_x100 / 100;
  const bool def = c.speed_ppm_x100 == 800000 && (c.flags & cal::kFlagDefaults);
  snprintf(out, cap, "%lu p/mi%s", ppm, def ? " (default)" : "");
}

void CardUi::statusText(char *out, size_t cap) const {
  const cal::Calibration &c = *_c.cal;
  snprintf(out, cap, "Fuel %s / Temp %s / Oil %s / Speed %s", cal::calibrated(c, cal::FUEL) ? "ok" : "not set",
           cal::calibrated(c, cal::TEMP) ? "ok" : "not set", cal::calibrated(c, cal::OIL) ? "ok" : "not set",
           c.speed_ppm_x100 == 800000 && (c.flags & cal::kFlagDefaults) ? "default" : "set");
}

void CardUi::text(uint8_t id, char *out, size_t cap) const {
  out[0] = 0;
  switch (id) {
    case 17: tableText(cal::FUEL, out, cap); break;
    case 37: tableText(cal::TEMP, out, cap); break;
    case 55: tableText(cal::OIL, out, cap); break;
    case 92: speedText(out, cap); break;
    case 100: statusText(out, cap); break;
    case 103: snprintf(out, cap, "%08lX", (unsigned long)_c.uid); break;
    default: break;
  }
}

bool CardUi::encodeValue(uint8_t index, ElcChWriter &w) {
  if (index >= kItemCount) return false;
  const ItemDef d = read_def(index);
  if (d.kind == GRP) return false;
  if (d.kind == TXT) {
    ElcUiText t;
    t.item = d.id;
    text(d.id, t.text, sizeof(t.text));
    elc_ui_encode(t, w);
  } else {
    ElcUiValue v;
    v.item = d.id;
    v.value = value(d.id);
    elc_ui_encode(v, w);
  }
  return w.ok();
}

// ---- sending

bool CardUi::sendItemValue(uint8_t id) {
  const int i = index_of(id);
  if (i < 0) return true;
  ElcChWriter w;
  if (!encodeValue((uint8_t)i, w)) return true;  // a group: nothing to send
  return _c.net->send(ELC_CH_UI, w);
}

void CardUi::sendResult(uint8_t item, uint8_t result, uint8_t asker) {
  ElcUiResult r;
  r.item = item;
  r.result = result;
  r.asker = asker;
  ElcChWriter w;
  elc_ui_encode(r, w);
  _c.net->send(ELC_CH_UI, w);
}

void CardUi::startRefresh(uint8_t group) {
  _refresh_group = group;
  _refresh_cursor = 0;
}

void CardUi::changed() {
  _dirty = true;
  _resend_settings = true;
  _resend_cursor = 0;
  if (_c.cal->flags & cal::kFlagDefaults) {
    _c.cal->flags &= (uint16_t)~cal::kFlagDefaults;
    if (_c.net) _c.net->setFactoryDefaults(false);
  }
}

void CardUi::resetCalibration() {
  cal::defaults(*_c.cal);
  _c.cal->flags |= cal::kFlagDefaults;
  for (auto &n : _table_note) n[0] = 0;
  _speed_note[0] = 0;
  _dirty = true;
  _resend_settings = true;
  _resend_cursor = 0;
  if (_c.net) _c.net->setFactoryDefaults(true);
}

// ---- SET

bool CardUi::setPoint(cal::ChannelCal &k, uint8_t ch, uint8_t slot, int32_t packed) {
  cal::ChannelCal t = k;
  if (packed == -1) {
    if (slot < t.n) {
      for (uint8_t i = slot; i + 1 < t.n; i++) t.pts[i] = t.pts[i + 1];
      t.n--;
    }
  } else {
    const uint16_t raw = (uint16_t)((uint32_t)packed >> 16);
    const int16_t val = (int16_t)(uint16_t)((uint32_t)packed & 0xFFFF);
    if (raw > 1023) return false;
    if (slot < t.n) {
      t.pts[slot].raw = raw;
      t.pts[slot].value = val;
    } else if (t.n < cal::max_points(ch)) {
      t.pts[t.n].raw = raw;
      t.pts[t.n].value = val;
      t.n++;
    } else {
      return false;
    }
  }
  if (!cal::monotonic(t)) return false;
  k = t;
  return true;
}

uint8_t CardUi::set(uint8_t id, int32_t v, uint8_t asker, bool *async) {
  cal::Calibration &c = *_c.cal;
  const ItemDef *d = def(id);
  if (!d) return ELC_UI_UNKNOWN;
  const uint8_t flags = pgm_read_byte(&d->flags), kind = pgm_read_byte(&d->kind);
  if (flags & ELC_UI_F_RO) return ELC_UI_READONLY;
  if (kind == GRP) return ELC_UI_UNKNOWN;
  int8_t e;
  int32_t mn, mx, st;
  rangeOf(id, &e, &mn, &mx, &st);
  if (kind == CHO) {
    uint8_t n = 2;
    if (id == 13 || id == 32 || id == 52) bias_table(channel_of(id), &n);
    else if (id == 15) n = cal::kFuelPresets;
    else if (id == 34) n = cal::kTempPresets;
    else if (id == 93) n = sizeof(kPpr);
    if (v < 0 || v >= n) return ELC_UI_RANGE;
  } else if (kind == NUM) {
    const bool point = (id >= 20 && id <= 27) || (id >= 40 && id <= 45) || (id >= 60 && id <= 63);
    if (!point && (v < mn || v > mx)) return ELC_UI_RANGE;
  }
  switch (id) {
    case 12: c.tank_x10 = (uint16_t)fromVolume(v); break;
    case 13: case 32: case 52: {
      uint8_t n;
      const uint32_t *t = bias_table(channel_of(id), &n);
      cal::channel(c, channel_of(id)).bias_x10 = pgm_read_dword(&t[v]);
      break;
    }
    case 14: c.fuel.topology = (uint8_t)v; break;
    case 33: c.temp.topology = (uint8_t)v; break;
    case 53: c.oil.topology = (uint8_t)v; break;
    case 15: c.fuel.preset = (uint8_t)v; break;
    case 34: c.temp.preset = (uint8_t)v; break;
    case 35: c.temp.offset = (int16_t)fromDelta(v); break;
    case 73: c.amb_offset = (int16_t)fromDelta(v); break;
    case 72: c.amb_ntc = (uint8_t)v; break;
    case 81: c.batt_trim_ppm = v * 100; break;
    case 90: c.speed_ppm_x100 = (uint32_t)v * 100; _speed_note[0] = 0; break;
    case 93: c.tach_ppr = kPpr[v]; break;
    case 16: case 36: case 54: {
      const uint8_t ch = id == 16 ? cal::FUEL : id == 36 ? cal::TEMP : cal::OIL;
      const int32_t canon = id == 16 ? fromVolume(v) : id == 36 ? fromTemp(v) : fromPress(v);
      if (!startCapture(ch, (int16_t)canon, asker, id)) return ELC_UI_BUSY;
      *async = true;
      return ELC_UI_OK;
    }
    case 82: {
      const int32_t trim = cal::battery_trim_for(_c.engine->lastBattRaw(), (float)v / 100.0f);
      c.batt_trim_ppm = trim;
      break;
    }
    case 18: case 38: case 56: {
      const uint8_t ch = id == 18 ? cal::FUEL : id == 38 ? cal::TEMP : cal::OIL;
      cal::clear_points(cal::channel(c, ch));
      _table_note[ch][0] = 0;
      break;
    }
    case 91:
      if (!startGpsCal(0)) return ELC_UI_BUSY;
      _gps.asker = asker;
      *async = true;
      return ELC_UI_OK;
    case 101: resetCalibration(); return ELC_UI_OK;
    case 102: _c.odo->set((uint32_t)v); return ELC_UI_OK;
    default:
      if (id >= 20 && id <= 27) {
        if (!setPoint(c.fuel, cal::FUEL, (uint8_t)(id - 20), v)) return ELC_UI_RANGE;
      } else if (id >= 40 && id <= 45) {
        if (!setPoint(c.temp, cal::TEMP, (uint8_t)(id - 40), v)) return ELC_UI_RANGE;
      } else if (id >= 60 && id <= 63) {
        if (!setPoint(c.oil, cal::OIL, (uint8_t)(id - 60), v)) return ELC_UI_RANGE;
      } else {
        return ELC_UI_UNKNOWN;
      }
      break;
  }
  changed();
  return ELC_UI_OK;
}

void CardUi::onTransfer(const ElcChTransfer &t, uint32_t now) {
  if (t.base != ELC_CH_UI || !_c.net) return;
  const uint8_t node = _c.net->node();
  if (t.op() == ELC_UI_SET) {
    ElcUiSet s;
    if (!elc_ui_decode(t, s) || !(s.dest == 0 || s.dest == node)) return;
    bool async = false;
    const uint8_t r = set(s.item, s.value, t.src, &async);
    if (async) return;  // the result comes when the capture is done
    sendResult(s.item, r, t.src);
    if (r == ELC_UI_OK) {
      const ItemDef *d = def(s.item);
      sendItemValue(s.item);  // the menu shows the value we answer with, never the one it asked for
      if (d) startRefresh(pgm_read_byte(&d->parent));  // readouts and the table follow
    }
  } else if (t.op() == ELC_UI_VIEW) {
    ElcUiView v;
    if (!elc_ui_decode(t, v) || !(v.dest == 0 || v.dest == node)) return;
    if (v.item == ELC_UI_VIEW_CLOSED) {
      _view = 0xFF;
      return;
    }
    if (_view != v.item && v.item != ELC_UI_VIEW_TOP) startRefresh(v.item);
    _view = v.item;
    _view_ms = now ? now : 1;
  }
}

// ---- captures

bool CardUi::startCapture(uint8_t ch, int16_t value, uint8_t asker, uint8_t item) {
  if (_cap.active) return false;
  _cap = Capture();
  _cap.active = true;
  _cap.ch = ch;
  _cap.value = value;
  _cap.asker = asker;
  _cap.item = item;
  _cap.start = 0xFFFFFFFFUL;  // set on the first poll
  _cap_msg[0] = 0;
  _cap_done = false;
  return true;
}

bool CardUi::captureDone(bool *ok) {
  if (!_cap_done) return false;
  _cap_done = false;
  if (ok) *ok = _cap_ok;
  return true;
}

void CardUi::finishCapture(uint32_t) {
  Capture &k = _cap;
  const float tol = k.ch == cal::FUEL ? 3.0f : 2.0f;
  bool ok = false;
  _cap_msg[0] = 0;
  if (k.n > 10 && (k.mx - k.mn) > 2.0f * tol) {
    snprintf(_cap_msg, sizeof(_cap_msg), "Unsteady, try again");
  } else if (!k.n) {
    snprintf(_cap_msg, sizeof(_cap_msg), "No reading, try again");
  } else {
    const uint16_t raw = (uint16_t)lroundf(k.sum / (float)k.n);
    ok = cal::capture(cal::channel(*_c.cal, k.ch), k.ch, raw, k.value);
    if (!ok) snprintf(_cap_msg, sizeof(_cap_msg), "Doesn't fit the other points: check the jumpers");
  }
  snprintf(_table_note[k.ch], sizeof(_table_note[k.ch]), "%s", _cap_msg);
  if (ok) changed();
  _cap_ok = ok;
  _cap_done = true;
  const uint8_t item = k.item, asker = k.asker;
  k.active = false;
  if (item != 0xFF && _c.net) {
    sendResult(item, ok ? ELC_UI_OK : ELC_UI_RANGE, asker);
    const ItemDef *d = def(item);
    if (d) startRefresh(pgm_read_byte(&d->parent));
  }
}

// ---- the speed calibration against the GPS card

bool CardUi::startGpsCal(uint32_t now) {
  if (_gps.active) return false;
  _gps = Gps();
  _gps.active = true;
  _gps.start = now ? now : 1;
  _gps.pulses0 = _c.engine->speedPulseTotal();
  snprintf(_speed_note, sizeof(_speed_note), "GPS: waiting for a fix");
  return true;
}

void CardUi::gpsFinish(bool apply, const char *msg) {
  snprintf(_speed_note, sizeof(_speed_note), "%s", msg);
  if (apply) changed();
  _gps.active = false;
  if (_c.net && _gps.asker) sendResult(91, apply ? ELC_UI_OK : ELC_UI_RANGE, _gps.asker);
  startRefresh(5);
}

void CardUi::onMotion(const ElcCanFrame &f, uint32_t now) {
  if (f.id != 0x223 || f.len < 7) return;
  const uint16_t cm_s = (uint16_t)(f.data[0] | (f.data[1] << 8));
  const bool fix3d = f.data[6] == 3;
  if (!_gps.active) return;
  Gps &g = _gps;
  if (!fix3d) {
    if (g.last_frame) gpsFinish(false, "GPS: lost the fix");
    return;
  }
  const float mph = (float)cm_s * 0.0223694f;
  if (mph < 25.0f) {  // slower than a steady highway run: restart the window
    g.start = now;
    g.pulses0 = _c.engine->speedPulseTotal();
    g.miles = 0;
    g.speed_min = g.speed_max = 0;
    g.last_frame = now;
    snprintf(_speed_note, sizeof(_speed_note), "GPS: hold 40-70 mph steady");
    return;
  }
  if (g.last_frame) {
    uint32_t dt = now - g.last_frame;
    if (dt > 1000) dt = 1000;
    g.miles += ((float)cm_s / 100.0f) * ((float)dt / 1000.0f) / 1609.344f;
  }
  g.last_frame = now;
  if (g.speed_max == 0) g.speed_min = g.speed_max = (float)cm_s;
  if ((float)cm_s < g.speed_min) g.speed_min = (float)cm_s;
  if ((float)cm_s > g.speed_max) g.speed_max = (float)cm_s;
  if (g.speed_max - g.speed_min > 0.01f * g.speed_max) {  // not steady: begin again from here
    g.start = now;
    g.pulses0 = _c.engine->speedPulseTotal();
    g.miles = 0;
    g.speed_min = g.speed_max = (float)cm_s;
    snprintf(_speed_note, sizeof(_speed_note), "GPS: speed not steady");
  } else {
    snprintf(_speed_note, sizeof(_speed_note), "GPS: measuring %lus", (unsigned long)((now - g.start) / 1000));
  }
  if (now - g.start >= 20000 && g.miles > 0.05f) {
    const uint32_t pulses = _c.engine->speedPulseTotal() - g.pulses0;
    const float ppm = (float)pulses / g.miles;
    const float now_ppm = (float)_c.cal->speed_ppm_x100 / 100.0f;
    char msg[32];
    if (ppm >= 500.0f && ppm <= 50000.0f && fabsf(ppm / now_ppm - 1.0f) <= 0.2f) {
      _c.cal->speed_ppm_x100 = (uint32_t)lroundf(ppm * 100.0f);
      snprintf(msg, sizeof(msg), "GPS: %lu p/mi, applied", (unsigned long)lroundf(ppm));
      gpsFinish(true, msg);
    } else {
      snprintf(msg, sizeof(msg), "GPS: %lu p/mi, off >20%%", (unsigned long)lroundf(ppm));
      gpsFinish(false, msg);
    }
  }
}

// ---- the pass through loop()

void CardUi::poll(uint32_t now) {
  if (!_c.net) return;
  // a capture averages the channel for 5 s at 20 Hz
  if (_cap.active) {
    Capture &k = _cap;
    if (k.start == 0xFFFFFFFFUL) {
      k.start = k.last = now;
    } else if (now - k.last >= kStepMs) {
      k.last = now;
      const float raw = _c.engine->lastRaw(k.ch);
      k.sum += raw;
      k.n++;
      if (k.n == 1) k.ema = raw;
      else k.ema += (raw - k.ema) / 8.0f;
      if (k.n == 11) k.mn = k.mx = k.ema;
      if (k.n > 11) {
        if (k.ema < k.mn) k.mn = k.ema;
        if (k.ema > k.mx) k.mx = k.ema;
      }
    }
    if (now - k.start >= 5000) finishCapture(now);
  }
  if (_gps.active && _gps.start && now - _gps.start > 120000UL) gpsFinish(false, "GPS: no steady run");

  // one send per pass, so the queue is never flooded and the periodic frames keep their turn
  if (_refresh_cursor >= 0) {
    while (_refresh_cursor < kItemCount) {
      const ItemDef d = read_def((uint8_t)_refresh_cursor);
      if (d.parent != _refresh_group || d.kind == GRP) {
        _refresh_cursor++;
        continue;
      }
      if (sendItemValue(d.id)) _refresh_cursor++;
      return;
    }
    _refresh_cursor = -1;
    return;
  }
  if (_resend_settings) {
    while (_resend_cursor < kItemCount && !isSetting(_resend_cursor)) _resend_cursor++;
    if (_resend_cursor >= kItemCount) {
      _resend_settings = false;
      return;
    }
    const ItemDef d = read_def(_resend_cursor);
    if (sendItemValue(d.id)) _resend_cursor++;
    return;
  }
  const bool viewed = _view != 0xFF && _view != ELC_UI_VIEW_TOP && _view_ms && now - _view_ms < 3000;
  if (viewed && _live_cursor == 0xFF && now - _live_ms >= 200) {  // live readouts of the open page, 5 Hz
    _live_ms = now;
    _live_cursor = 0;
  }
  if (_live_cursor != 0xFF) {
    while (_live_cursor < kItemCount) {
      const ItemDef d = read_def(_live_cursor);
      if (d.parent != _view || !(d.flags & ELC_UI_F_LIVE)) {
        _live_cursor++;
        continue;
      }
      if (sendItemValue(d.id)) _live_cursor++;
      return;
    }
    _live_cursor = 0xFF;
  }
}
