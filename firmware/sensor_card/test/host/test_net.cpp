// Host tests for the card's channel side: NET, SENSOR, ALERT, the menu (UI), and the legacy frames. A fake center
// plays the other end through the real framing code.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "card_chan.h"
#include "card_net.h"
#include "card_ui.h"
#include "legacy.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define NEAR(a, b, tol) do { double _a = (a), _b = (b); if (fabs(_a - _b) > (tol)) { printf("FAIL %s:%d  %s=%g want %g\n", __FILE__, __LINE__, #a, _a, _b); g_fail++; } } while (0)

struct FakeFram {
  uint8_t mem[2048];
  FakeFram() { memset(mem, 0xFF, sizeof(mem)); }
};
static bool f_probe(void *) { return true; }
static bool f_read(void *c, uint16_t a, uint8_t *b, uint8_t n) { memcpy(b, ((FakeFram *)c)->mem + a, n); return true; }
static bool f_write(void *c, uint16_t a, const uint8_t *b, uint8_t n) { memcpy(((FakeFram *)c)->mem + a, b, n); return true; }

// what the card sent, reassembled the way the center would
struct Sent {
  uint16_t base;
  uint8_t src;
  std::vector<uint8_t> data;
  uint32_t at;
};

struct Rig {
  FakeFram fram;
  cal::Calibration cal;
  Odometer odo;
  Engine engine;
  EngineOut out;
  CardNet net;
  CardUi ui;
  CardChan chan;
  std::vector<ElcCanFrame> wire;
  std::vector<Sent> sent;
  ElcChRx rx;
  uint32_t now = 1000;
  uint8_t room = 200;
  static Rig *self;

  static bool tx(const ElcCanFrame &f) {
    self->wire.push_back(f);
    ElcChTransfer t;
    if (self->rx.feed(f, self->now, t)) {
      Sent s;
      s.base = t.base;
      s.src = t.src;
      s.data.assign(t.data, t.data + t.len);
      s.at = self->now;
      self->sent.push_back(s);
    }
    return true;
  }
  static uint8_t freeRoom() { return self->room; }
  static void onTransfer(const ElcChTransfer &t) { self->ui.onTransfer(t, self->now); }
  static void onDescribe(uint8_t what) { self->chan.describe(what); }

  Rig() {
    self = this;
    cal::defaults(cal);
    FramIo io{f_probe, f_read, f_write, &fram};
    odo.begin(io, 1000);
    odo.tryLoad();
    engine.begin(&cal, &odo);
    ElcNetAnnounce me;
    me.type = ELC_TYPE_SENSOR;
    me.product = ELC_PRODUCT_SENSOR_CARD;
    me.uid = 0xCAFE0004;
    me.flags = 1;
    me.produces = elc_ch_bit(ELC_CH_SENSOR) | elc_ch_bit(ELC_CH_UI) | elc_ch_bit(ELC_CH_ALERT);
    strcpy(me.name, "Sensor card");
    CardNet::Hooks h;
    h.transfer = onTransfer;
    h.describe = onDescribe;
    net.begin(0x04, me, tx, freeRoom, h);
    CardUi::Ctx uc;
    uc.cal = &cal;
    uc.engine = &engine;
    uc.odo = &odo;
    uc.net = &net;
    uc.out = &out;
    uc.uid = 0xCAFE0004;
    ui.begin(uc);
    CardChan::Ctx cc;
    cc.net = &net;
    cc.ui = &ui;
    cc.out = &out;
    cc.cal = &cal;
    chan.begin(cc, now);
    EngineIn in;
    in.raw[0] = 236;
    in.raw[1] = 500;
    in.raw[2] = 44;
    in.raw[3] = 511.5f;
    in.batt_raw = 512;
    in.ign = true;
    in.alt_high = true;
    stepEngine(in);
  }
  void stepEngine(const EngineIn &in) { engine.step(in, out); }
  void run(uint32_t ms, uint32_t tick = 5) {
    const uint32_t end = now + ms;
    while (now < end) {
      now += tick;
      net.poll(now);
      chan.poll(now);
      ui.poll(now);
    }
  }
  /** One transfer from the center (node 1) to the card. */
  void fromCenter(uint16_t base, const uint8_t *p, size_t n) {
    ElcCanFrame fr[ELC_CH_MAX_FRAMES];
    static uint8_t tid = 0;
    const size_t k = elc_ch_frames((uint16_t)(base + 1), p, n, tid++, fr, ELC_CH_MAX_FRAMES);
    for (size_t i = 0; i < k; i++) net.feed(fr[i], now);
  }
  void set(uint8_t item, int32_t value) {
    ElcUiSet s;
    s.dest = 0x04;
    s.item = item;
    s.value = value;
    ElcChWriter w;
    elc_ui_encode(s, w);
    fromCenter(ELC_CH_UI, w.data(), w.size());
  }
  void view(uint8_t item) {
    ElcUiView v;
    v.dest = 0x04;
    v.item = item;
    ElcChWriter w;
    elc_ui_encode(v, w);
    fromCenter(ELC_CH_UI, w.data(), w.size());
  }
  int count(uint16_t base, uint8_t op) const {
    int n = 0;
    for (const Sent &s : sent) if (s.base == base && !s.data.empty() && s.data[0] == op) n++;
    return n;
  }
  const Sent *last(uint16_t base, uint8_t op, int from = 0) const {
    const Sent *r = nullptr;
    for (size_t i = (size_t)from; i < sent.size(); i++) if (sent[i].base == base && sent[i].data[0] == op) r = &sent[i];
    return r;
  }
};
Rig *Rig::self = nullptr;

static bool result_of(const Sent *s, ElcUiResult *r) {
  if (!s) return false;
  ElcChTransfer t;
  t.base = s->base;
  t.src = s->src;
  t.data = s->data.data();
  t.len = s->data.size();
  return elc_ui_decode(t, *r);
}

static void test_boot_and_descriptions() {
  Rig r;
  r.run(3000);
  // ANNOUNCE first
  CHECK(r.count(ELC_CH_NET, ELC_NET_ANNOUNCE) == 1);
  const Sent *a = r.last(ELC_CH_NET, ELC_NET_ANNOUNCE);
  ElcNetAnnounce an;
  CHECK(a && elc_net_decode_announce(a->data.data(), a->data.size(), an));
  CHECK(an.type == ELC_TYPE_SENSOR && an.product == 0x0008 && an.uid == 0xCAFE0004 && an.flags == 1);
  CHECK(strcmp(an.name, "Sensor card") == 0);
  CHECK(a->src == 4);
  // twelve sensors, the alert, and all 64 items, each with a label
  CHECK(r.count(ELC_CH_SENSOR, ELC_SENSOR_DEFINE) == 12);
  CHECK(r.count(ELC_CH_ALERT, ELC_ALERT_DEFINE) == 1);
  CHECK(r.count(ELC_CH_UI, ELC_UI_ITEM) == 64);
  bool seen[256] = {false};
  int groups = 0;
  for (const Sent &s : r.sent) {
    if (s.base != ELC_CH_UI || s.data[0] != ELC_UI_ITEM) continue;
    ElcChTransfer t;
    t.base = s.base; t.src = s.src; t.data = s.data.data(); t.len = s.data.size();
    ElcUiItem it;
    CHECK(elc_ui_decode(t, it));
    CHECK(!seen[it.item]);
    seen[it.item] = true;
    CHECK(strlen(it.label) > 0);
    if (it.kind == ELC_UI_GROUP) groups++;
    else CHECK(seen[it.parent] || it.parent < 7);
    if (it.kind == ELC_UI_CHOICE) CHECK(it.option_count() >= 2);
  }
  CHECK(groups == 7);
  // the settings' values follow, for the center's backup (not the readouts)
  const int values = r.count(ELC_CH_UI, ELC_UI_VALUE);
  CHECK(values >= 30 && values < 45);
  // paced: descriptions are at least 8 ms apart, never a burst
  uint32_t prev = 0;
  for (const Sent &s : r.sent) {
    if (s.base == ELC_CH_UI && s.data[0] == ELC_UI_ITEM) {
      if (prev) CHECK(s.at - prev >= 8);
      prev = s.at;
    }
  }
  // ALERT STATE is on as soon as the key is on and temp / oil are not calibrated
  const Sent *st = r.last(ELC_CH_ALERT, ELC_ALERT_STATE);
  CHECK(st && st->data.size() >= 6 && (st->data[1] & 1));
}

static void test_query_and_prefs() {
  Rig r;
  r.run(3000);
  const size_t before = r.sent.size();
  const uint8_t q[3] = {ELC_NET_QUERY, 0x00, ELC_NET_Q_ANNOUNCE | ELC_NET_Q_SENSORS};
  const uint32_t t0 = r.now;
  r.fromCenter(ELC_CH_NET, q, 3);
  r.run(20);
  CHECK(r.count(ELC_CH_NET, ELC_NET_ANNOUNCE) == 1);  // not yet: node x 8 ms = 32 ms
  r.run(400);
  bool announced = false;
  for (size_t i = before; i < r.sent.size(); i++) {
    if (r.sent[i].base == ELC_CH_NET && r.sent[i].data[0] == ELC_NET_ANNOUNCE) { announced = true; CHECK(r.sent[i].at - t0 >= 32); }
  }
  CHECK(announced);
  int defs = 0;
  for (size_t i = before; i < r.sent.size(); i++) if (r.sent[i].base == ELC_CH_SENSOR && r.sent[i].data[0] == ELC_SENSOR_DEFINE) defs++;
  CHECK(defs == 12);
  // a query addressed to someone else is ignored
  const size_t mid = r.sent.size();
  const uint8_t q2[3] = {ELC_NET_QUERY, 0x15, ELC_NET_Q_ALL};
  r.fromCenter(ELC_CH_NET, q2, 3);
  r.run(200);
  CHECK(r.sent.size() - mid < 20);  // the SENSOR values only

  // PREFS: metric / degC / kPa changes the units, so the generation moves and the center is told again
  const uint8_t gen = r.net.generation();
  r.ui.setUnits(7);
  CHECK(r.net.generation() != gen);
  r.run(2000);
  const Sent *tank = nullptr;
  for (const Sent &s : r.sent) {
    if (s.base == ELC_CH_UI && s.data[0] == ELC_UI_ITEM && s.data[1] == 12) tank = &s;  // the newest
  }
  ElcUiItem it;
  ElcChTransfer t;
  t.base = tank->base; t.src = tank->src; t.data = tank->data.data(); t.len = tank->data.size();
  CHECK(elc_ui_decode(t, it));
  CHECK(strcmp(it.unit, "L") == 0 && it.min == 189 && it.max == 1514);
}

static void test_set() {
  Rig r;
  r.run(3000);
  r.sent.clear();
  r.ui.setUnits(0);  // gallons
  r.run(2500);
  r.sent.clear();
  r.set(12, 250);  // 25.0 gal
  NEAR(r.cal.tank_x10, 946, 1);
  ElcUiResult res;
  CHECK(result_of(r.last(ELC_CH_UI, ELC_UI_RESULT), &res));
  CHECK(res.item == 12 && res.result == ELC_UI_OK && res.asker == 1);
  const Sent *v = r.last(ELC_CH_UI, ELC_UI_VALUE);
  CHECK(v);
  ElcChTransfer t;
  t.base = v->base; t.src = v->src; t.data = v->data.data(); t.len = v->data.size();
  ElcUiValue uv;
  CHECK(elc_ui_decode(t, uv) && uv.item == 12 && uv.value == 250);  // the value it answers with, in the menu's units
  CHECK(r.ui.takeDirty());
  CHECK(!(r.cal.flags & cal::kFlagDefaults));  // no longer at the factory defaults
  // range, read-only, unknown
  r.sent.clear();
  r.set(12, 5);
  CHECK(result_of(r.last(ELC_CH_UI, ELC_UI_RESULT), &res) && res.result == ELC_UI_RANGE);
  r.set(10, 5);
  CHECK(result_of(r.last(ELC_CH_UI, ELC_UI_RESULT), &res) && res.result == ELC_UI_READONLY);
  r.set(99, 5);
  CHECK(result_of(r.last(ELC_CH_UI, ELC_UI_RESULT), &res) && res.result == ELC_UI_UNKNOWN);
  r.set(13, 9);  // no such bias option
  CHECK(result_of(r.last(ELC_CH_UI, ELC_UI_RESULT), &res) && res.result == ELC_UI_RANGE);
  // bias choice, wiring and preset
  r.set(13, 1);
  CHECK(r.cal.fuel.bias_x10 == 2200);
  r.set(14, 1);
  CHECK(r.cal.fuel.topology == cal::PULL_DOWN);
  r.set(15, 3);
  CHECK(r.cal.fuel.preset == 3);
  r.set(32, 2);
  CHECK(r.cal.temp.bias_x10 == 100000);
  r.set(34, 3);
  CHECK(r.cal.temp.preset == 3);
  // degF offset +9.0 F is +5.0 C
  r.set(35, 90);
  CHECK(r.cal.temp.offset == 50);
  // pulses per mile, tach ppr
  r.set(90, 8123);
  CHECK(r.cal.speed_ppm_x100 == 812300);
  r.set(93, 4);
  CHECK(r.cal.tach_ppr == 6);
  // a point set directly (the center's restore): raw 44 -> 0 kPa then raw 460 -> 700 kPa
  r.set(60, (44 << 16) | 0);
  r.set(61, (460 << 16) | 700);
  CHECK(r.cal.oil.n == 2);
  r.set(62, (200 << 16) | 900);  // breaks monotonic order: refused
  CHECK(result_of(r.last(ELC_CH_UI, ELC_UI_RESULT), &res) && res.result == ELC_UI_RANGE);
  CHECK(r.cal.oil.n == 2);
  r.set(60, -1);  // remove the first
  CHECK(r.cal.oil.n == 1);
  // set odometer
  r.set(102, 12345);
  CHECK(r.odo.tenths() == 12345);
  // reset
  r.set(101, 1);
  CHECK((r.cal.flags & cal::kFlagDefaults) && r.cal.speed_ppm_x100 == 800000 && r.cal.oil.n == 0);
}

static void test_capture() {
  Rig r;
  r.run(3000);
  r.ui.setUnits(0);  // psi
  r.sent.clear();
  // oil, engine off, raw 44 counts: "Oil pressure is now: 0 psi"
  EngineIn in;
  in.raw[0] = 236; in.raw[1] = 500; in.raw[2] = 44; in.raw[3] = 511.5f; in.batt_raw = 512;
  r.stepEngine(in);
  r.set(54, 0);
  ElcUiResult res;
  CHECK(!result_of(r.last(ELC_CH_UI, ELC_UI_RESULT), &res));  // busy for 5 s: no answer yet
  CHECK(r.ui.capturing());
  r.set(54, 0);  // a second one while busy
  CHECK(result_of(r.last(ELC_CH_UI, ELC_UI_RESULT), &res) && res.result == ELC_UI_BUSY);
  r.sent.clear();
  for (int i = 0; i < 110; i++) { r.stepEngine(in); r.run(50, 5); }
  CHECK(result_of(r.last(ELC_CH_UI, ELC_UI_RESULT), &res) && res.item == 54 && res.result == ELC_UI_OK && res.asker == 1);
  CHECK(r.cal.oil.n == 1 && r.cal.oil.pts[0].raw == 44 && r.cal.oil.pts[0].value == 0);
  // 100 psi (689 kPa) at raw 460
  in.raw[2] = 460;
  r.stepEngine(in);
  r.set(54, 1000);  // 100.0 psi
  r.sent.clear();
  for (int i = 0; i < 110; i++) { r.stepEngine(in); r.run(50, 5); }
  CHECK(r.cal.oil.n == 2);
  NEAR(r.cal.oil.pts[1].value, 689, 1);
  // now the channel reads: raw 252 is half way (about 345 kPa = 50 psi)
  in.raw[2] = 252;
  r.stepEngine(in);
  NEAR(r.out.oil_kpa, 345, 3);
  CHECK(r.out.st_oil == cal::ST_OK);
  // an unsteady reading is refused, and says why
  Rig u;
  u.run(3000);
  u.sent.clear();
  u.ui.setUnits(0);
  u.set(54, 0);
  for (int i = 0; i < 110; i++) {
    EngineIn w = in;
    w.raw[2] = (i & 1) ? 60.0f : 30.0f;
    u.stepEngine(w);
    u.run(50, 5);
  }
  CHECK(result_of(u.last(ELC_CH_UI, ELC_UI_RESULT), &res) && res.result == ELC_UI_RANGE);
  CHECK(u.cal.oil.n == 0);
  bool ok = true;
  CHECK(u.ui.captureDone(&ok) && !ok);
  CHECK(strstr(u.ui.captureMessage(), "Unsteady"));
  // a capture that breaks the table is refused too ("check the jumpers")
  Rig m;
  m.run(3000);
  cal::capture(m.cal.oil, cal::OIL, 44, 0);
  cal::capture(m.cal.oil, cal::OIL, 460, 700);
  EngineIn z = in;
  z.raw[2] = 252;
  m.stepEngine(z);
  m.ui.setUnits(7);  // kPa
  m.sent.clear();
  CHECK(m.ui.startCapture(cal::OIL, 900, 1, 54));  // 900 kPa at the mid point, above the 700 at 460 counts
  for (int i = 0; i < 110; i++) { m.stepEngine(z); m.run(50, 5); }
  CHECK(result_of(m.last(ELC_CH_UI, ELC_UI_RESULT), &res) && res.result == ELC_UI_RANGE);
  CHECK(m.cal.oil.n == 2);
  CHECK(strstr(m.ui.captureMessage(), "jumpers"));
}

static void test_view_and_live() {
  Rig r;
  r.run(3000);
  r.sent.clear();
  r.view(1);  // the engine temperature page opens
  r.run(3000, 5);
  // the page's values arrive at once, then its live readouts keep coming for as long as VIEW is repeated
  int temp_vals = 0;
  for (const Sent &s : r.sent) {
    if (s.base == ELC_CH_UI && s.data[0] == ELC_UI_VALUE && s.data[1] == 30) temp_vals++;
  }
  CHECK(temp_vals >= 10 && temp_vals <= 20);  // once at once, then 5 Hz for the 3 s the VIEW stays fresh
  r.sent.clear();
  r.run(1000, 5);
  CHECK(r.count(ELC_CH_UI, ELC_UI_VALUE) == 0);  // and nothing after that: the page is not being repeated
  r.sent.clear();
  for (int i = 0; i < 20; i++) {
    r.view(1);
    r.run(100, 5);
  }
  int live = 0, other = 0;
  for (const Sent &s : r.sent) {
    if (s.base == ELC_CH_UI && s.data[0] == ELC_UI_VALUE) {
      if (s.data[1] == 30 || s.data[1] == 31) live++;
      else other++;
    }
  }
  CHECK(live >= 18 && live <= 24);  // two live items at 5 Hz for 2 s = about 20
  CHECK(other == 0);                // nothing from other pages
  // closed: it stops
  r.view(0xFF);
  r.sent.clear();
  r.run(1000, 5);
  CHECK(r.count(ELC_CH_UI, ELC_UI_VALUE) == 0);
}

static void test_sensor_values() {
  Rig r;
  r.run(2500);
  r.sent.clear();
  r.out.mph = 60.0f;
  r.out.engine_rpm = 3000;
  r.out.fuel_pct10 = 750;
  r.out.st_fuel = cal::ST_OK;
  r.out.temp_c10 = 905;
  r.out.st_temp = cal::ST_OK;
  r.out.oil_kpa = 310;
  r.out.st_oil = cal::ST_OK;
  r.out.batt_v100 = 1385;
  r.out.odo_tenths = 123456;
  r.out.st_odo = cal::ST_OK;
  r.out.mpg_inst_x10 = 150;
  r.out.dte_tenths = 2000;
  r.out.charging = true;
  r.run(2000, 5);
  int n[12] = {0};
  for (const Sent &s : r.sent) {
    if (s.base != ELC_CH_SENSOR || s.data[0] != ELC_SENSOR_VALUE) continue;
    ElcChTransfer t;
    t.base = s.base; t.src = s.src; t.data = s.data.data(); t.len = s.data.size();
    ElcSensorValue v;
    CHECK(elc_sensor_decode(t, v) && v.sensor < 12);
    CHECK(s.src == 4);
    n[v.sensor]++;
    switch (v.sensor) {
      case 0: NEAR(v.value, 965, 1); CHECK(v.status == 0); break;      // 60 mph = 96.5 km/h
      case 1: CHECK(v.value == 3000); break;
      case 2: CHECK(v.value == 750); break;
      case 3: CHECK(v.value == 905); break;
      case 4: CHECK(v.value == 310); break;
      case 5: CHECK(v.value == 1); break;
      case 7: NEAR(v.value, 123456 * 160.9344, 2); break;              // metres
      case 8: NEAR(v.value, 2000 * 160.9344, 2); break;
      case 9: NEAR(v.value, 150 * 4.251437, 1); break;                 // km/L x100
      case 10: CHECK(v.status == ELC_SENSOR_NOT_READY); break;         // no trip economy yet
      case 11: CHECK(v.value == 1385); break;
      default: break;
    }
  }
  // cadence: speed and rpm 20 Hz, the gauges 5 Hz, the slow ones 1 Hz
  NEAR(n[0], 40, 3);
  NEAR(n[1], 40, 3);
  NEAR(n[2], 10, 2);
  NEAR(n[4], 10, 2);
  NEAR(n[6], 2, 1);
  NEAR(n[11], 2, 1);
  // ...and nothing is sent when the queue has no room (a transfer is all or nothing)
  r.sent.clear();
  r.room = 0;
  r.run(500, 5);
  CHECK(r.sent.empty());
  // the alert follows the key and the calibration
  r.room = 200;
  r.out.ign = true;
  r.run(1200, 5);
  const Sent *st = r.last(ELC_CH_ALERT, ELC_ALERT_STATE);
  CHECK(st && (st->data[1] & 1));
  cal::capture(r.cal.temp, cal::TEMP, 100, 0);
  cal::capture(r.cal.temp, cal::TEMP, 400, 1000);
  cal::capture(r.cal.oil, cal::OIL, 44, 0);
  cal::capture(r.cal.oil, cal::OIL, 460, 700);
  r.sent.clear();
  r.run(1200, 5);
  st = r.last(ELC_CH_ALERT, ELC_ALERT_STATE);
  CHECK(st && !(st->data[1] & 1));
}

static void test_legacy() {
  EngineOut o;
  o.pot_adc = 0x0123; o.therm_adc = 0x0234; o.pressure_adc = 0x0345; o.charge_adc = 720; o.ambient_adc = 0x0111;
  o.wheel_rpm = 1000; o.engine_rpm = 3500; o.flags = 3; o.faults = 0x8004; o.odo_tenths = 966487;
  o.mpg_inst_x10 = 155; o.mpg_avg_x10 = 171; o.dte_tenths = 2500; o.has_econ = true;
  uint8_t u[kUartFrameLen];
  legacy_uart_frame(o, u);
  CHECK(u[0] == 0xAA && u[1] == 0x55);
  CHECK(u[2] == 0x01 && u[3] == 0x23 && u[4] == 0x02 && u[5] == 0x34);  // big-endian ADCs
  CHECK(u[12] == 0 && u[13] == 0 && u[14] == 0x03 && u[15] == 0xE8);    // wheel rpm at offset 12
  CHECK(u[16] == 0 && u[17] == 0 && u[18] == 0x0D && u[19] == 0xAC);    // engine rpm at 16
  CHECK(u[20] == 3);                                                    // flags at 20
  CHECK(u[21] == 0x80 && u[22] == 0x04);                                // fault word at 21
  CHECK(u[23] == 0 && u[24] == 0x0E && u[25] == 0xBF && u[26] == 0x57); // odometer at 23: 966487
  CHECK(u[27] == 0 && u[28] == 155);                                    // mpg now at 27
  uint8_t x = 0;
  for (int i = 2; i < 35; i++) x ^= u[i];
  CHECK(u[35] == x);
  // the CAN frames carry the same fields
  ElcCanFrame f[ELC_CAN_SENSOR_FRAMES];
  legacy_can_frames(o, 7, f);
  ElcCanSensorDecoder d;
  for (int i = 0; i < 5; i++) CHECK(d.feed(f[i]));
  CHECK(d.complete());
  const ElcCanSensor &s = d.value();
  CHECK(s.pot_adc == 0x0123 && s.speed_rpm == 1000 && s.engine_rpm == 3500 && s.faults == 0x8004);
  CHECK(s.odometer_tenths_mile == 966487 && s.mpg_average_x10 == 171 && s.has_fuel_economy && s.seq == 7);
}

int main() {
  test_boot_and_descriptions();
  test_query_and_prefs();
  test_set();
  test_capture();
  test_view_and_live();
  test_sensor_values();
  test_legacy();
  if (g_fail) {
    printf("%d failure(s)\n", g_fail);
    return 1;
  }
  printf("net tests passed\n");
  return 0;
}
