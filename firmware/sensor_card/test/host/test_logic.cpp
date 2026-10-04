// Host tests for the card's logic: calibration, odometer + FRAM blocks, the engine. No hardware.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "calib.h"
#include "engine.h"
#include "odometer.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define NEAR(a, b, tol) do { double _a = (a), _b = (b); if (fabs(_a - _b) > (tol)) { printf("FAIL %s:%d  %s=%g want %g\n", __FILE__, __LINE__, #a, _a, _b); g_fail++; } } while (0)

// ---- a FRAM in an array
struct FakeFram {
  uint8_t mem[2048];
  bool present = true;
  int writes = 0;
  FakeFram() { memset(mem, 0xFF, sizeof(mem)); }
};
static bool f_probe(void *c) { return ((FakeFram *)c)->present; }
static bool f_read(void *c, uint16_t a, uint8_t *b, uint8_t n) {
  FakeFram *f = (FakeFram *)c;
  if (!f->present) return false;
  memcpy(b, f->mem + a, n);
  return true;
}
static bool f_write(void *c, uint16_t a, const uint8_t *b, uint8_t n) {
  FakeFram *f = (FakeFram *)c;
  if (!f->present) return false;
  memcpy(f->mem + a, b, n);
  f->writes++;
  return true;
}
static FramIo io(FakeFram &f) { return FramIo{f_probe, f_read, f_write, &f}; }

static void test_conversions() {
  using namespace cal;
  Calibration c;
  defaults(c);
  // fuel, as delivered: pull-up 100 ohm, GM 0-30 ohm. Counts = 1023 * R / (R + 100)
  NEAR(raw_to_ohms(c.fuel, 236.0f), 30.0f, 0.3);
  NEAR(raw_to_ohms(c.fuel, 123.0f), 13.7, 0.2);
  Reading r = convert(c, FUEL, 236.0f);  // full
  CHECK(r.status == ST_OK);
  NEAR(r.value, c.tank_x10, 2);
  r = convert(c, FUEL, 0.0f);  // empty reads 0 counts and is NOT a short for a 0-30 ohm sender
  CHECK(r.status == ST_OK);
  CHECK(r.value <= 2);
  r = convert(c, FUEL, 1020.0f);
  CHECK(r.status == ST_OPEN);
  // temperature and oil are not calibrated as delivered
  CHECK(convert(c, TEMP, 500.0f).status == ST_NOT_READY);
  CHECK(convert(c, OIL, 200.0f).status == ST_NOT_READY);
  CHECK(convert(c, OIL, 1020.0f).status == ST_OPEN);  // faults still show
  // an oil sender with calibration points: 44 -> 0 kPa, 460 -> 700 kPa  (10-180 ohm sender, 220 ohm bias)
  CHECK(capture(c.oil, OIL, 44, 0));
  CHECK(capture(c.oil, OIL, 460, 700));
  r = convert(c, OIL, 252.0f);
  CHECK(r.status == ST_OK);
  NEAR(r.value, 350, 1);
  r = convert(c, OIL, 1.0f);  // short: the lowest point is above 20 counts
  CHECK(r.status == ST_SHORT);
  // a point that breaks monotonicity is refused and the table is unchanged
  CHECK(!capture(c.oil, OIL, 300, 900));
  CHECK(c.oil.n == 2);
  // a point within 3 counts replaces
  CHECK(capture(c.oil, OIL, 461, 710));
  CHECK(c.oil.n == 2 && c.oil.pts[1].value == 710);
  // temperature presets: GM coolant sensor ~3.4k at 70 F (21 C) and 185 ohm at 210 F
  NEAR(gm_coolant_celsius(3400.0f), (70 - 32) * 5.0 / 9, 0.1);
  NEAR(gm_coolant_celsius(185.0f), (210 - 32) * 5.0 / 9, 0.1);
  NEAR(ntc_celsius(100000.0f, 100000.0f), 25.0, 0.01);
  NEAR(ntc_celsius(10000.0f, 10000.0f), 25.0, 0.01);
  c.temp.preset = 3;
  c.temp.bias_x10 = 2200;  // 220 ohm, pull-up: 185 ohm sender -> 1023*185/405
  r = convert(c, TEMP, 1023.0f * 185.0f / 405.0f);
  CHECK(r.status == ST_OK);
  NEAR(r.value, 989, 8);
  c.temp.offset = 20;  // +2.0 C
  NEAR(convert(c, TEMP, 1023.0f * 185.0f / 405.0f).value, 1009, 8);
  // one point shifts the preset through it
  c.temp.offset = 0;
  CHECK(capture(c.temp, TEMP, 466, 1000));
  NEAR(convert(c, TEMP, 466.0f).value, 1000, 1);
  // ambient: 100k NTC at 25 C is mid-scale
  r = convert_ambient(c, 511.5f);
  CHECK(r.status == ST_OK);
  NEAR(r.value, 250, 2);
  // battery: 1023 counts of ADC8 = 27.7 V, trim round trip
  NEAR(battery_volts(c, 512.0f), 13.88, 0.02);
  const int32_t trim = battery_trim_for(512.0f, 14.2f);
  c.batt_trim_ppm = trim;
  NEAR(battery_volts(c, 512.0f), 14.2, 0.01);
}

static void test_points_and_storage() {
  using namespace cal;
  ChannelCal k;
  k.n = 0;
  for (int i = 0; i < 12; i++) capture(k, FUEL, (uint16_t)(20 * i + 10), (int16_t)(50 * i));
  CHECK(k.n == 8);  // a full table replaces the nearest
  CHECK(monotonic(k));

  Calibration c;
  defaults(c);
  c.flags = 0;
  c.fuel.bias_x10 = 2200;
  c.fuel.preset = 2;
  c.tank_x10 = 600;
  c.batt_trim_ppm = -1234;
  c.speed_ppm_x100 = 799900;
  c.tach_ppr = 6;
  CHECK(capture(c.oil, OIL, 44, 0) && capture(c.oil, OIL, 460, 700));
  uint8_t blk[kBlockMax];
  const size_t n = block_encode(c, 77, blk, sizeof(blk));
  CHECK(n > 40);
  Calibration d;
  uint32_t seq = 0;
  CHECK(block_decode(blk, n, &seq, d));
  CHECK(seq == 77);
  CHECK(d.flags == 0 && d.fuel.bias_x10 == 2200 && d.fuel.preset == 2 && d.tank_x10 == 600);
  CHECK(d.batt_trim_ppm == -1234 && d.speed_ppm_x100 == 799900 && d.tach_ppr == 6);
  CHECK(d.oil.n == 2 && d.oil.pts[1].raw == 460 && d.oil.pts[1].value == 700);
  blk[20] ^= 1;  // a flipped bit fails the CRC
  CHECK(!block_decode(blk, n, &seq, d));
  // a short payload (an older firmware's) keeps the defaults for what is missing
  uint8_t pl[kPayloadMax];
  const size_t pn = serialize(c, pl, sizeof(pl));
  Calibration e;
  CHECK(deserialize(pl, pn - 5, e));  // lost the speed and tach fields
  CHECK(e.speed_ppm_x100 == 800000 && e.tach_ppr == 0 && e.tank_x10 == 600);
  // and extra trailing bytes are ignored
  uint8_t pl2[kPayloadMax + 8];
  memcpy(pl2, pl, pn);
  memset(pl2 + pn, 0x5A, 8);
  CHECK(deserialize(pl2, pn + 8, e) && e.tach_ppr == 6);

  // the store alternates copies and picks the newest valid one
  FakeFram f;
  CalStore s;
  s.begin(io(f));
  Calibration g;
  CHECK(!s.load(g));  // blank
  CHECK(s.save(c));
  c.tank_x10 = 650;
  CHECK(s.save(c));
  CalStore s2;
  s2.begin(io(f));
  CHECK(s2.load(g) && g.tank_x10 == 650 && s2.seq() == 2);
  f.mem[kFramCalB + 12] ^= 0xFF;  // corrupt the newest copy: the older one wins
  CalStore s3;
  s3.begin(io(f));
  CHECK(s3.load(g) && g.tank_x10 == 600 && s3.seq() == 1);
}

static void test_odometer() {
  FakeFram f;
  Odometer o;
  o.begin(io(f), 966486UL);
  CHECK(o.tryLoad() == OdoLoad::Ok);  // blank: seeded
  CHECK(o.tenths() == 966486UL && o.known());
  CHECK(f.writes == 1);
  o.add(150000000UL);  // 0.15 mile: crosses one tenth
  CHECK(o.tenths() == 966487UL && o.fracNano() == 50000000UL);
  o.stampFrac();
  Odometer p;  // a reboot
  p.begin(io(f), 1);
  CHECK(p.tryLoad() == OdoLoad::Ok);
  CHECK(p.tenths() == 966487UL && p.fracNano() == 50000000UL);
  // power loss mid-write: the newest slot is torn, the other one is still good
  const uint8_t slot_b = 0x30, slot_a = 0x10;
  (void)slot_a;
  f.mem[slot_b + 9] ^= 0x55;  // whichever was written last
  f.mem[slot_a + 9] ^= 0x00;
  Odometer q;
  q.begin(io(f), 1);
  CHECK(q.tryLoad() == OdoLoad::Ok);
  // a chip that does not answer: unknown, nothing written
  FakeFram n;
  n.present = false;
  Odometer u;
  u.begin(io(n), 100);
  CHECK(u.tryLoad() == OdoLoad::NoChip);
  CHECK(!u.known());
  u.add(200000000UL);  // distance while unknown is kept
  n.present = true;
  CHECK(u.tryLoad() == OdoLoad::Ok);  // blank chip: the seed, plus the 0.2 mile driven
  CHECK(u.tenths() == 102);
  // garbage that is not blank: never seeded
  FakeFram g;
  memset(g.mem + 0x10, 0x12, 40);
  Odometer v;
  v.begin(io(g), 100);
  CHECK(v.tryLoad() == OdoLoad::Corrupt);
  CHECK(!v.known());
  // a legacy v2 blob migrates
  FakeFram h;
  uint8_t l[12] = {0xE1, 0xD0, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // 65536 tenths
  memcpy(h.mem, l, 12);
  Odometer w;
  w.begin(io(h), 5);
  CHECK(w.tryLoad() == OdoLoad::Ok && w.tenths() == 65536);
  // the identity block
  FakeFram i;
  uint32_t uid = 0;
  CHECK(!identity_read(io(i), &uid));
  CHECK(identity_write(io(i), 0xA1B2C3D4));
  CHECK(identity_read(io(i), &uid) && uid == 0xA1B2C3D4);
}

static EngineIn steady(float fuel_raw, bool ign) {
  EngineIn in;
  in.raw[0] = fuel_raw;
  in.raw[1] = 500;
  in.raw[2] = 252;
  in.raw[3] = 511.5f;
  in.batt_raw = 512;
  in.ign = ign;
  in.alt_high = ign;
  return in;
}

static void test_engine() {
  cal::Calibration c;
  cal::defaults(c);
  FakeFram f;
  Odometer o;
  o.begin(io(f), 1000);
  o.tryLoad();
  Engine e;
  e.begin(&c, &o);
  EngineOut out;
  EngineIn in = steady(236.0f, true);
  e.step(in, out);
  CHECK(out.st_fuel == cal::ST_OK);
  NEAR(out.fuel_pct10, 1000, 3);
  CHECK(out.pot_adc >= 1020);  // the screens' straight line shows full
  CHECK(out.st_temp == cal::ST_NOT_READY && out.therm_adc == 0);  // uncalibrated: 0 -> the screens' out-of-range
  CHECK(out.st_oil == cal::ST_NOT_READY && out.pressure_adc == 0);
  CHECK(out.charge_adc == 720 && (out.flags & 3) == 3);
  NEAR(out.batt_v100, 1388, 3);

  // fuel slosh is damped: a sudden dip moves the gauge by a sliver, not at once
  in.raw[0] = 118.0f;
  for (int i = 0; i < 20; i++) e.step(in, out);  // 1 s
  CHECK(out.fuel_pct10 > 900);
  // a refuel (engine off) is followed after 10 s
  c.fuel.preset = 1;
  Engine e2;
  e2.begin(&c, &o);
  EngineIn low = steady(30.0f, false);
  for (int i = 0; i < 5; i++) e2.step(low, out);
  const int32_t was = out.fuel_pct10;
  EngineIn full = steady(236.0f, false);
  for (int i = 0; i < 100; i++) e2.step(full, out);  // 5 s: still catching up slowly
  CHECK(out.fuel_pct10 < 600 && out.fuel_pct10 > was);
  for (int i = 0; i < 120; i++) e2.step(full, out);  // 10 s of it held: snapped
  CHECK(out.fuel_pct10 > 950);

  // speed: 8000 pulses/mile, 22.2 pulses/s = 10 mph  -> period 45045 us
  Engine e3;
  e3.begin(&c, &o);
  EngineIn m = steady(236.0f, true);
  m.speed_period_us = 45000;
  m.speed_since_us = 1000;
  m.tach_period_us = 7500;  // 8 ppr: 8000 pulses/min -> 1000 rpm
  m.tach_since_us = 100;
  e3.step(m, out);
  NEAR(out.mph, 10.0, 0.1);
  NEAR(out.engine_rpm, 1000, 2);
  // the screens' 2.1 m wheel formula returns the true speed
  const double mph_screen = out.wheel_rpm / 60.0 * 2.1 * 2.236936;
  NEAR(mph_screen, 10.0, 0.1);
  // slowing: with no pulse for a long while the reading falls
  m.speed_since_us = 400000;
  e3.step(m, out);
  CHECK(out.mph < 1.5);
  m.speed_since_us = 2500000;
  e3.step(m, out);
  CHECK(out.mph == 0);

  // distance: 8000 pulses is one mile = 10 tenths
  const uint32_t t0 = o.tenths();
  m.speed_pulses = 0;
  e3.step(m, out);
  m.speed_pulses = 4000;
  e3.step(m, out);
  m.speed_pulses = 8000;
  e3.step(m, out);
  CHECK(o.tenths() - t0 == 10);

  // faults: an open fuel wire is debounced for 500 ms, then flagged
  EngineIn open = steady(1020.0f, true);
  Engine e4;
  e4.begin(&c, &o);
  for (int i = 0; i < 9; i++) e4.step(open, out);
  CHECK(!(out.faults & FAULT_POT_OPEN));
  e4.step(open, out);
  CHECK(out.faults & FAULT_POT_OPEN);
  CHECK(out.st_fuel == cal::ST_OPEN);
  // a tripped feed monitor is reported as the feed, not as an open sender
  EngineIn feed = steady(236.0f, true);
  feed.temp_feed_ok = false;
  feed.raw[1] = 1020.0f;
  for (int i = 0; i < 12; i++) e4.step(feed, out);
  CHECK((out.faults & FAULT_TEMP_POWER) && !(out.faults & FAULT_TEMP_OPEN));
  CHECK(out.st_temp == cal::ST_FAULT);
  // coil missing: ignition on, no pulses for 5 s
  EngineIn coil = steady(236.0f, true);
  Engine e5;
  e5.begin(&c, &o);
  for (int i = 0; i < 99; i++) e5.step(coil, out);
  CHECK(!(out.faults & FAULT_COIL_MISSING));
  e5.step(coil, out);
  CHECK(out.faults & FAULT_COIL_MISSING);

  // no FRAM chip: the odometer is flagged not stored
  FakeFram none;
  none.present = false;
  Odometer on;
  on.begin(io(none), 1000);
  on.tryLoad();
  Engine e6;
  e6.begin(&c, &on);
  e6.step(coil, out);
  CHECK(out.faults & FAULT_ODO_STORE);
  CHECK(out.odo_tenths == kOdoUnknown);
  on.set(1234);  // bench: set in RAM
  e6.step(coil, out);
  CHECK(out.odo_tenths == 1234 && (out.faults & FAULT_ODO_STORE));  // known, but not being saved

  // legacy coolant counts invert the screens' formula
  const uint16_t cnt = Engine::legacyCoolantCounts(90.0f);
  const float r = 100000.0f * (1023.0f / cnt - 1.0f);
  const float s = logf(r / 100000.0f) / 3950.0f + 1.0f / 298.15f;
  NEAR(1.0f / s - 273.15f, 90.0, 0.6);

  // the demo sweep fills everything and stays in range
  Engine d;
  d.begin(&c, &o);
  for (int i = 0; i < 1300; i++) {
    d.demo(out);
    CHECK(out.pot_adc <= 1023 && out.pressure_adc <= 1023 && out.therm_adc <= 1023);
  }
}

int main() {
  test_conversions();
  test_points_and_storage();
  test_odometer();
  test_engine();
  if (g_fail) {
    printf("%d failure(s)\n", g_fail);
    return 1;
  }
  printf("logic tests passed\n");
  return 0;
}
