// The bench console on USART1 (J7, 3.3 V), 115200: set up a card before it goes in the car, read what it sees, and
// type a sender's datasheet curve in directly (docs/protocols/sensor_calibration.md, Bench console). Rev A's console
// (../sensor_card/src/console.cpp) with: `demo on|off` in place of the J9 jumper, no `can cs` / `can osc` (the CAN
// controller is built in), the FDCAN counters, and plain strings (no flash-string macros: RAM is not short).
// Console text and the USB link's records share the port; this reads only the text (elc_usb_read).
//   US units on the console (gal, degF, psi, mph) unless suffixed: L, C, kPa.
#include <Arduino.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "adc_scale.h"
#include "card_pins.h"
#include "card_state.h"
#include "elc_usb_link.h"
#include "hw_adc.h"
#include "hw_sys.h"

namespace {
char g_line[56];
uint8_t g_len = 0;
uint32_t g_mile_pulses = 0;
bool g_mile_on = false;

void say(const char *fmt, ...) {
  char b[120];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.println(b);
}
#define SAY(f, ...) say(f, ##__VA_ARGS__)

const char *status_name(uint8_t st) {
  switch (st) {
    case cal::ST_OK: return "ok";
    case cal::ST_NOT_READY: return "not calibrated";
    case cal::ST_OPEN: return "open";
    case cal::ST_SHORT: return "short";
    case cal::ST_RANGE: return "range";
    default: return "fault";
  }
}

/** "12.5" -> 125 with one decimal. `suffix` points at what follows the number. */
bool parse_x10(const char *s, int32_t *out, const char **suffix) {
  bool neg = false;
  if (*s == '-') { neg = true; s++; }
  else if (*s == '+') s++;
  if (*s < '0' || *s > '9') return false;
  int32_t v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  v *= 10;
  if (*s == '.') {
    s++;
    if (*s >= '0' && *s <= '9') v += *s - '0';
    while (*s >= '0' && *s <= '9') s++;
  }
  *out = neg ? -v : v;
  if (suffix) *suffix = s;
  return true;
}

bool parse_int(const char *s, int32_t *out) {
  int32_t t;
  const char *e;
  if (!parse_x10(s, &t, &e) || *e) return false;
  *out = t / 10;
  return true;
}

/** Ohms from "220", "1k", "100k", "4.7k" (x10). */
bool parse_ohms_x10(const char *s, uint32_t *out) {
  int32_t t;
  const char *e;
  if (!parse_x10(s, &t, &e) || t < 0) return false;
  uint32_t v = (uint32_t)t;
  if (*e == 'k' || *e == 'K') v *= 1000;
  *out = v;
  return v > 0;
}

int channel_index(const char *n) {
  if (!strcmp(n, "fuel")) return cal::FUEL;
  if (!strcmp(n, "temp")) return cal::TEMP;
  if (!strcmp(n, "oil")) return cal::OIL;
  if (!strcmp(n, "amb")) return cal::AMB;
  return -1;
}

/** A value in the channel's console units to canonical (L x10, degC x10, kPa). */
bool canonical_value(uint8_t ch, const char *s, int32_t *out) {
  int32_t v;
  const char *suf;
  if (!parse_x10(s, &v, &suf)) return false;
  if (ch == cal::FUEL) *out = (*suf == 'L' || *suf == 'l') ? v : (int32_t)lroundf((float)v * 3.785412f);
  else if (ch == cal::OIL) *out = (!strcmp(suf, "kPa") || !strcmp(suf, "kpa")) ? v / 10 : (int32_t)lroundf((float)v / 10.0f * 6.894757f);
  else *out = (*suf == 'C' || *suf == 'c') ? v : (int32_t)lroundf(((float)v - 320.0f) / 1.8f);
  return true;
}

void print_channel(uint8_t ch) {
  static const char names[3][5] = {"fuel", "temp", "oil"};
  const cal::ChannelCal &k = cal::channel(card.cal, ch);
  const float raw = card.engine.lastRaw(ch);
  const cal::Reading r = cal::convert(card.cal, ch, raw);
  const long b10 = (long)k.bias_x10;
  SAY("%s: %s, bias %ld.%ld ohm, preset %u, offset %d, %u point(s); raw %d -> %ld (%s)", names[ch],
      k.topology == cal::PULL_UP ? "pull-up" : "pull-down", b10 / 10, b10 % 10, (unsigned)k.preset, (int)k.offset,
      (unsigned)k.n, (int)lroundf(raw), (long)r.value, status_name(r.status));
  for (uint8_t i = 0; i < k.n; i++) SAY("    %u: raw %u -> %d", (unsigned)(i + 1), (unsigned)k.pts[i].raw, (int)k.pts[i].value);
}

void cmd_cal(int argc, char **argv) {
  cal::Calibration &c = card.cal;
  if (argc == 1) {
    for (uint8_t ch = 0; ch < 3; ch++) print_channel(ch);
    const cal::Reading a = cal::convert_ambient(c, card.engine.lastRaw(3));
    SAY("amb: %s NTC, offset %d; raw %d -> %ld (%s)", c.amb_ntc ? "10k" : "100k", (int)c.amb_offset,
        (int)lroundf(card.engine.lastRaw(3)), (long)a.value, status_name(a.status));
    SAY("tank %ld (L x10), battery trim %ld ppm, speed %lu p/mi, tach %u ppr (%s)", (long)c.tank_x10, (long)c.batt_trim_ppm,
        (unsigned long)(c.speed_ppm_x100 / 100), (unsigned)card.tach_ppr, c.tach_ppr ? "set" : "J5");
    return;
  }
  const char *w = argv[1];
  if (!strcmp(w, "reset")) {
    card.ui.resetCalibration();
    SAY("calibration reset to the delivered defaults");
    return;
  }
  if (!strcmp(w, "save")) {
    SAY("%s", card_save_calibration() ? "saved" : "no FRAM, nothing saved");
    return;
  }
  if (!strcmp(w, "export") || !strcmp(w, "import")) {
    SAY("not implemented: the center's SD card backs the settings up");
    return;
  }
  if (!strcmp(w, "batt") && argc >= 3) {
    int32_t v;
    if (!parse_x10(argv[2], &v, nullptr)) return SAY("volts?");
    c.batt_trim_ppm = cal::battery_trim_for(card.engine.lastBattRaw(), (float)v / 10.0f);
    card.ui.changed();
    SAY("battery trim %ld ppm", (long)c.batt_trim_ppm);
    return;
  }
  if (!strcmp(w, "speed")) {
    if (argc >= 4 && !strcmp(argv[2], "ppm")) {
      int32_t v;
      if (!parse_int(argv[3], &v) || v < 500 || v > 50000) return SAY("500 to 50000");
      c.speed_ppm_x100 = (uint32_t)v * 100;
      card.ui.changed();
      SAY("speed %ld pulses per mile", (long)v);
    } else if (argc >= 3 && !strcmp(argv[2], "mile") && argc >= 4 && !strcmp(argv[3], "start")) {
      g_mile_pulses = card.in.speed_pulses;
      g_mile_on = true;
      SAY("counting speed pulses: drive a measured distance, then `cal speed mile stop <miles>`");
    } else if (argc >= 5 && !strcmp(argv[2], "mile") && !strcmp(argv[3], "stop") && g_mile_on) {
      int32_t m10;
      if (!parse_x10(argv[4], &m10, nullptr) || m10 <= 0) return SAY("miles?");
      const uint32_t pulses = card.in.speed_pulses - g_mile_pulses;
      const uint32_t ppm = (uint32_t)((uint64_t)pulses * 10ULL / (uint32_t)m10);
      g_mile_on = false;
      if (ppm < 500 || ppm > 50000) return SAY("%lu pulses in that distance: %lu p/mi, out of range", (unsigned long)pulses, (unsigned long)ppm);
      c.speed_ppm_x100 = ppm * 100;
      card.ui.changed();
      SAY("%lu pulses: %lu pulses per mile", (unsigned long)pulses, (unsigned long)ppm);
    } else {
      SAY("cal speed ppm <n> | cal speed mile start | stop <miles>");
    }
    return;
  }
  if (!strcmp(w, "tach")) {
    if (argc >= 4 && !strcmp(argv[2], "ppr")) {
      int32_t v = 0;
      if (!strcmp(argv[3], "j5")) v = 0;
      else if (!parse_int(argv[3], &v) || v < 0 || v > 16) return SAY("1-16 or j5");
      c.tach_ppr = (uint8_t)v;
      card.ui.changed();
      SAY("tach %u pulses per revolution", (unsigned)v);
    }
    return;
  }
  const int ch = channel_index(w);
  if (ch < 0 || argc < 3) return SAY("cal [fuel|temp|oil|amb] ...: wiring pu|pd, bias <ohms>, preset <n>, offset <v>, capture <v>, point <raw|ohmsR> <v>, clear; cal fuel tank <gal>; cal batt <volts>; cal speed ...; cal tach ppr <n|j5>; cal reset|save");
  const char *what = argv[2];
  if (ch == cal::AMB) {
    if (!strcmp(what, "ntc") && argc >= 4) {
      c.amb_ntc = !strcmp(argv[3], "10") || !strcmp(argv[3], "10k");
    } else if (!strcmp(what, "offset") && argc >= 4) {
      int32_t v;
      const char *suf;
      if (!parse_x10(argv[3], &v, &suf)) return SAY("offset?");
      c.amb_offset = (int16_t)((*suf == 'C' || *suf == 'c') ? v : lroundf((float)v / 1.8f));
    } else {
      return SAY("cal amb ntc 100|10, cal amb offset <v>");
    }
    card.ui.changed();
    return;
  }
  cal::ChannelCal &k = cal::channel(c, (uint8_t)ch);
  if (!strcmp(what, "wiring") && argc >= 4) {
    k.topology = !strcmp(argv[3], "pd") ? cal::PULL_DOWN : cal::PULL_UP;
  } else if (!strcmp(what, "bias") && argc >= 4) {
    uint32_t b;
    if (!parse_ohms_x10(argv[3], &b)) return SAY("ohms?");
    k.bias_x10 = b;
  } else if (!strcmp(what, "preset") && argc >= 4) {
    int32_t n;
    if (!parse_int(argv[3], &n) || n < 0 || (ch == cal::FUEL ? n >= cal::kFuelPresets : ch == cal::TEMP ? n >= cal::kTempPresets : n > 0))
      return SAY("no such preset");
    k.preset = (uint8_t)n;
  } else if (!strcmp(what, "offset") && argc >= 4 && ch == cal::TEMP) {
    int32_t v;
    const char *suf;
    if (!parse_x10(argv[3], &v, &suf)) return SAY("offset?");
    k.offset = (int16_t)((*suf == 'C' || *suf == 'c') ? v : lroundf((float)v / 1.8f));
  } else if (!strcmp(what, "clear")) {
    cal::clear_points(k);
  } else if (!strcmp(what, "capture") && argc >= 4) {
    int32_t v;
    if (!canonical_value((uint8_t)ch, argv[3], &v)) return SAY("value?");
    if (!card.ui.startCapture((uint8_t)ch, (int16_t)v)) return SAY("a capture is running");
    SAY("averaging for 5 s...");
    return;
  } else if (!strcmp(what, "point") && argc >= 5) {
    int32_t v;
    if (!canonical_value((uint8_t)ch, argv[4], &v)) return SAY("value?");
    int32_t raw;
    const size_t n = strlen(argv[3]);
    if (n && (argv[3][n - 1] == 'R' || argv[3][n - 1] == 'r')) {  // sender ohms: converted with the bias and wiring
      char t[12];
      strncpy(t, argv[3], sizeof(t) - 1);
      t[sizeof(t) - 1] = 0;
      t[strlen(t) - 1] = 0;
      uint32_t ohms10;
      if (!parse_ohms_x10(t, &ohms10)) return SAY("ohms?");
      raw = (int32_t)lroundf(cal::ohms_to_raw(k, (float)ohms10 / 10.0f));
    } else if (!parse_int(argv[3], &raw) || raw < 0 || raw > 1023) {
      return SAY("raw 0-1023, or <ohms>R");
    }
    if (!cal::capture(k, (uint8_t)ch, (uint16_t)raw, (int16_t)v)) return SAY("does not fit the other points");
    SAY("point raw %ld -> %ld", (long)raw, (long)v);
  } else if (ch == cal::FUEL && !strcmp(what, "tank") && argc >= 4) {
    int32_t v;
    if (!canonical_value(cal::FUEL, argv[3], &v) || v < 189 || v > 1514) return SAY("5 to 40 gal");
    c.tank_x10 = (uint16_t)v;
  } else {
    return SAY("?");
  }
  card.ui.changed();
  print_channel((uint8_t)ch);
}

void cmd_can() {
  const FdCanStats &s = card.can.stats();
  static const char *const states[3] = {"active", "passive", "BUS-OFF"};
  static const char *const errs[8] = {"none", "stuff", "form", "ack", "bit1", "bit0", "crc", "?"};
  if (card.can.running()) {
    SAY("can: FDCAN1 up at %u kbit/s, error %s (tec %u rec %u, last %s); tx sent %lu acked %lu dropped %lu, rx %lu lost %lu over %lu, bus-off %lu",
        (unsigned)ELC_CAN_BITRATE_KBPS, states[card.can.errorState() > 2 ? 2 : card.can.errorState()],
        (unsigned)card.can.txErrors(), (unsigned)card.can.rxErrors(), errs[card.can.lastError() & 7],
        (unsigned long)s.tx_sent, (unsigned long)s.tx_ok, (unsigned long)s.tx_dropped, (unsigned long)s.rx,
        (unsigned long)s.rx_lost, (unsigned long)s.rx_over, (unsigned long)s.bus_off);
  } else {
    SAY("can: FDCAN1 did not start (the HAL refused the configuration)");
  }
  SAY("net: node 0x%02X, transfers in %lu out %lu dropped %lu", (unsigned)card.net.node(),
      (unsigned long)card.net.transfersIn(), (unsigned long)card.net.transfersOut(), (unsigned long)card.net.dropped());
  char u[100];
  elc_usb_describe(u, sizeof(u));
  Serial.println(u);
}

void cmd_demo(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "on")) card.demo = true;
  else if (argc >= 2 && !strcmp(argv[1], "off")) card.demo = false;
  else if (argc >= 2) return SAY("demo on|off");
  SAY("demo %s (a slow fake sweep of everything, bench only; off again at restart)", card.demo ? "on" : "off");
}

void cmd_status() {
  const EngineOut &o = card.out;
  SAY("sensor card rev B fw %d, node 0x%02X, uid %08lX, up %lus, clock %s, reset cause %02X%s", CARD_FW_VERSION,
      (unsigned)kCardNode, (unsigned long)card.uid, (unsigned long)(millis() / 1000),
      sys_clock_is_hse() ? "HSE 8 MHz" : "HSI (crystal failed)", (unsigned)sys_reset_cause(), card.demo ? ", DEMO" : "");
  SAY("fram %s, calibration %s%s, odometer %s%s", card.fram ? "ok" : "none", card.cal_from_fram ? "from FRAM" : "defaults",
      card.cal_dirty ? " (unsaved)" : "", card.odo.known() ? "known" : "unknown", card.odo.persisted() ? "" : " (not stored)");
  SAY("ign %u alt %u | %d mph x10 | %lu rpm | fuel %ld.%ld%% (%s) | temp %ld (%s) | oil %ld kPa (%s) | batt %ld.%02ld V",
      (unsigned)o.ign, (unsigned)(o.flags & 1), (int)lroundf(o.mph * 10.0f), (unsigned long)o.engine_rpm,
      (long)(o.fuel_pct10 / 10), (long)(o.fuel_pct10 % 10), status_name(o.st_fuel), (long)o.temp_c10, status_name(o.st_temp),
      (long)o.oil_kpa, status_name(o.st_oil), (long)(o.batt_v100 / 100), (long)(o.batt_v100 % 100));
  SAY("odometer %lu (tenths of a mile), faults %04X, mpg x10 now %u trip %u, dte %lu", (unsigned long)o.odo_tenths,
      (unsigned)o.faults, (unsigned)o.mpg_inst_x10, (unsigned)o.mpg_avg_x10, (unsigned long)o.dte_tenths);
}

void cmd_adc() {
  // rev A's scale (what the calibration sees), then the raw 12-bit samples and the level inputs in slot volts
  SAY("counts: fuel %d  temp %d  oil %d  amb %d  batt %d  (feeds: temp %u oil %u)", (int)card.engine.lastRaw(0),
      (int)card.engine.lastRaw(1), (int)card.engine.lastRaw(2), (int)card.engine.lastRaw(3),
      (int)card.engine.lastBattRaw(), (unsigned)card.in.temp_feed_ok, (unsigned)card.in.press_feed_ok);
  static const char *const names[SLOT_COUNT] = {"fuel", "temp", "oil", "amb", "alt", "batt", "wake", "ign"};
  char line[120];
  size_t n = snprintf(line, sizeof(line), "raw12:");
  for (uint8_t i = 0; i < SLOT_COUNT && n < sizeof(line); i++)
    n += snprintf(line + n, sizeof(line) - n, " %s %u", names[i], (unsigned)adc_raw12((AdcSlot)i));
  Serial.println(line);
  const float ign = adc_slot_volts(adc_raw12(SLOT_IGN), kIgnDivTop, kIgnDivBottom);
  const float alt = adc_slot_volts(adc_raw12(SLOT_ALT), kAltDivTop, kAltDivBottom);
  const float bat = adc_slot_volts(adc_raw12(SLOT_BATT), kBattDivTop, kBattDivBottom);
  SAY("slot volts: ign %d.%d (%s)  alt %d.%d (%s)  batt %d.%d  wake pin %d.%02d", (int)ign, (int)(ign * 10) % 10,
      card.in.ign ? "on" : "off", (int)alt, (int)(alt * 10) % 10, card.in.alt_high ? "high" : "low", (int)bat,
      (int)(bat * 10) % 10, (int)adc_pin_volts(adc_raw12(SLOT_WAKE)), (int)(adc_pin_volts(adc_raw12(SLOT_WAKE)) * 100) % 100);
}

void cmd_odo(int argc, char **argv) {
  if (argc >= 3 && !strcmp(argv[1], "set")) {
    int32_t t;
    if (!parse_x10(argv[2], &t, nullptr) || t < 0) return SAY("miles?");
    card.odo.set((uint32_t)t);
    SAY("odometer %ld.%ld mi%s", (long)(t / 10), (long)(t % 10), card.odo.persisted() ? "" : " (no FRAM: kept in RAM only)");
    return;
  }
  if (card.odo.known()) {
    SAY("odometer %lu.%lu mi (+%lu nano-mi)%s", (unsigned long)(card.odo.tenths() / 10), (unsigned long)(card.odo.tenths() % 10),
        (unsigned long)card.odo.fracNano(), card.odo.persisted() ? "" : ", not stored");
  } else {
    SAY("odometer unknown (FRAM unreadable)");
  }
}

void help() {
  SAY("status | can | adc | cal | odo | demo on|off | restart");
  SAY("cal [fuel|temp|oil|amb] ...   cal reset|save   odo set <miles>");
}

void run(char *line) {
  char *argv[6];
  int argc = 0;
  for (char *p = strtok(line, " \t"); p && argc < 6; p = strtok(nullptr, " \t")) argv[argc++] = p;
  if (!argc) return;
  const char *c = argv[0];
  if (!strcmp(c, "help") || !strcmp(c, "?")) help();
  else if (!strcmp(c, "status")) cmd_status();
  else if (!strcmp(c, "can")) cmd_can();
  else if (!strcmp(c, "demo")) cmd_demo(argc, argv);
  else if (!strcmp(c, "adc")) cmd_adc();
  else if (!strcmp(c, "cal")) cmd_cal(argc, argv);
  else if (!strcmp(c, "odo")) cmd_odo(argc, argv);
  else if (!strcmp(c, "restart")) card_restart();
  else SAY("? (help)");
}
}  // namespace

void console_begin() { Serial.println("sensor card rev B: `help` for commands"); }

void console_poll(uint32_t now) {
  (void)now;
  bool ok;
  if (card.ui.captureDone(&ok)) SAY("%s", ok ? "capture saved" : card.ui.captureMessage());
  int c;
  int budget = 64;
  while (budget-- > 0 && (c = elc_usb_read()) >= 0) {
    if (c == '\r' || c == '\n') {
      g_line[g_len] = 0;
      g_len = 0;
      run(g_line);
    } else if (g_len < sizeof(g_line) - 1 && c >= 0x20) {
      g_line[g_len++] = (char)c;
    }
  }
}
