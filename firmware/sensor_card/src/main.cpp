// DigiDash sensor card firmware (SensorCard/, ATmega2560, CAN node 0x04).
//
// Reads the senders, speed, tach, ignition, battery and alternator, keeps the odometer and the calibration in the FRAM,
// and sends the gauges' data on CAN (frames 0x100-0x104 at 20 Hz, the SENSOR channel, the card's menu page on UI) and,
// as the fallback path, on the J8 serial as the 36-byte frames. The USB link (tools/can_relay.py) and the bench console
// share USART0 (J7 FTDI, or an Arduino Mega's USB). Docs: firmware/sensor_card/README.md.
#include <Arduino.h>
#include <avr/wdt.h>

#include "card_pins.h"
#include "card_state.h"
#include "elc_chan_msg.h"
#include "elc_usb_link.h"
#include "fram.h"
#include "hw_adc.h"
#include "hw_capture.h"
#include "legacy.h"

#ifndef ODO_INITIAL_TENTHS
#define ODO_INITIAL_TENTHS 966486UL  // 96648.6 mi: written only into a blank FRAM; set the car's mileage here
#endif

Card card;

// The reset cause (MCUSR), saved before anything else runs: a watchdog reset leaves the watchdog enabled at its shortest
// timeout (15 ms), so it is cleared and the watchdog stopped in .init3, before the C runtime's start-up and the Arduino
// core's init(), or the card would reset again before setup() could reach it. `status` prints it.
uint8_t card_reset_cause __attribute__((section(".noinit")));
extern "C" void card_wdt_off() __attribute__((naked, used, section(".init3")));
extern "C" void card_wdt_off() {
  card_reset_cause = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

namespace {
const uint32_t kHoldAfterOffMs = 3000;  // WAKE stays up this long after key-off, for the FRAM work and the other cards
const uint32_t kSaveMinGapMs = 1000;    // calibration is written at most once a second
const uint32_t kBannerFirstMs = 300, kBannerGapMs = 500, kBootNoteMs = 6000;

uint32_t g_last_step = 0, g_last_hb = 0, g_last_odo_retry = 0, g_last_ign_on = 0;
uint8_t g_seq = 0, g_ign_cnt = 0;
bool g_ign_cand = false, g_ign_state = false, g_flush_pending = false;
bool g_banner_odo_sent = false, g_boot_note_sent = false;
uint8_t g_banner_next = 0;
uint32_t g_banner_at = 0;
char g_boot_l1[20] = {0}, g_boot_l2[20] = {0};

bool tx_frame(const ElcCanFrame &f) {
  const bool usb = elc_usb_send_frame(f);
  const bool bus = card.can.running() && card.can.send(f);
  return usb || bus;
}
uint8_t tx_free() { return card.can.running() ? card.can.txFree() : 255; }

void on_transfer(const ElcChTransfer &t) {
  const uint32_t now = millis();
  if (t.base == ELC_CH_UI) {
    card.ui.onTransfer(t, now);
  } else if (t.base == ELC_CH_SYSTEM) {
    ElcSysPrefs p;
    ElcSysPower pw;
    if (t.op() == ELC_SYS_PREFS && elc_sys_decode(t, p)) card.ui.setUnits(p.units);
    else if (t.op() == ELC_SYS_POWER && elc_sys_decode(t, pw) && pw.event == ELC_POWER_IGN_OFF) g_flush_pending = true;
  }
}

void on_describe(uint8_t what) { card.chan.describe(what); }

void on_net_cmd(uint8_t op, const uint8_t *p, size_t n) {
  if (op == ELC_NET_IDENTIFY && n >= 3) {
    card.identify_until = p[2] ? millis() + (uint32_t)p[2] * 1000UL : 0;
  } else if (op == ELC_NET_RESTART && n >= 3 && p[2] == ELC_NET_RESTART_MAGIC) {
    card_restart();
  }
}

void deliver(const ElcCanFrame &f, uint32_t now) {
  if (f.id == 0x213 || f.id == 0x701) card.center_ms = now ? now : 1;
  if (f.id == 0x223) card.ui.onMotion(f, now);
  card.net.feed(f, now);
}

uint32_t make_uid() {  // the 2560 has no serial number: noise from the ADC and the timer
  uint32_t x = micros() ^ ((uint32_t)TCNT0 << 24);
  for (uint8_t i = 0; i < 64; i++) {
    x = (x << 1) | (x >> 31);
    x ^= (uint32_t)(analogRead(i & 3) & 1) ^ (micros() & 1);
    delayMicroseconds(37);
  }
  return x ? x : 0xA5C3F00D;
}

bool uart_note_pending() { return g_boot_l1[0] != 0; }

/** The J8 serial's text, as the old sensor pack sent it: a five-line animation, then the odometer, then what the FRAM
 *  said at boot. The screens show it in the odometer slot and log it. Non-blocking: frames keep flowing between lines. */
void uart_banner(uint32_t now) {
  static const char anim[5][7] PROGMEM = {"------", "--<>--", "-<##>-", "<####>", "######"};
  if (g_banner_next < 5 && (int32_t)(now - g_banner_at) >= 0) {
    char line[8];
    memcpy_P(line, anim[g_banner_next], 7);
    Serial1.println(line);
    g_banner_next++;
    g_banner_at = now + kBannerGapMs;
  } else if (g_banner_next == 5 && !g_banner_odo_sent && (int32_t)(now - g_banner_at) >= 0) {
    char buf[8];
    if (card.odo.known()) {
      uint32_t o = card.odo.tenths();
      snprintf(buf, sizeof(buf), "%06lu", (unsigned long)(o > 999999UL ? 999999UL : o));
    } else {
      snprintf(buf, sizeof(buf), "??????");
    }
    Serial1.println(buf);
    g_banner_odo_sent = true;
  }
  const bool link = card.center_ms && now - card.center_ms < 1000;
  if (!g_boot_note_sent && g_banner_odo_sent && uart_note_pending() && (link || now >= kBootNoteMs)) {
    Serial1.println(g_boot_l1);
    if (g_boot_l2[0]) Serial1.println(g_boot_l2);
    g_boot_note_sent = true;
  }
}

void note_boot_load(OdoLoad r) {
  const char *n = card.odo.note();
  if (r == OdoLoad::Ok) {
    snprintf(g_boot_l1, sizeof(g_boot_l1), "%s", n[0] ? n : "FRAM rd OK");
    snprintf(g_boot_l2, sizeof(g_boot_l2), "t=%lu", (unsigned long)card.odo.tenths());
  } else {
    snprintf(g_boot_l1, sizeof(g_boot_l1), "FRAM rd FAIL");
    snprintf(g_boot_l2, sizeof(g_boot_l2), "%s", r == OdoLoad::Corrupt ? "corrupt" : "no chip");
  }
}

uint8_t resolve_tach_ppr() {
  if (card.cal.tach_ppr) return card.cal.tach_ppr;
  const bool a = digitalRead(PIN_RPM_SEL_A) == LOW, b = digitalRead(PIN_RPM_SEL_B) == LOW;
  // Pulses per crankshaft revolution: a single-coil 4-stroke fires cylinders / 2 times a revolution
  if (a && !b) return 2;  // 4 cylinders
  if (!a && b) return 3;  // 6 cylinders
  return 4;  // none, or both: the V8 default
}

void sample_inputs(EngineIn &in, uint32_t dt) {
  float v;
  static const AdcSlot slots[4] = {SLOT_FUEL, SLOT_TEMP, SLOT_OIL, SLOT_AMB};
  for (uint8_t i = 0; i < 4; i++) {
    if (adc_take(slots[i], &v)) in.raw[i] = v;
  }
  if (adc_take(SLOT_BATT, &v)) in.batt_raw = v;
  float wake;
  adc_take(SLOT_WAKE, &wake);  // keeps the scan's slot drained; WAKE is only shown by `adc`
  const bool ign_raw = digitalRead(PIN_IGN) == HIGH;
  if (ign_raw == g_ign_cand) {
    if (g_ign_cnt < 255) g_ign_cnt++;
  } else {
    g_ign_cand = ign_raw;
    g_ign_cnt = 0;
  }
  if (g_ign_cnt >= 2) g_ign_state = g_ign_cand;
  in.ign = g_ign_state;
  in.alt_high = digitalRead(PIN_ALT_L) == HIGH;
  in.temp_feed_ok = digitalRead(PIN_TEMP_MON) == HIGH;
  in.press_feed_ok = digitalRead(PIN_PRESS_MON) == HIGH;
  const PulseInfo sp = capture_speed(), tp = capture_tach();
  in.speed_period_us = sp.period_us;
  in.speed_since_us = sp.since_us;
  in.speed_pulses = sp.count;
  in.tach_period_us = tp.period_us;
  in.tach_since_us = tp.since_us;
  card.tach_ppr = resolve_tach_ppr();
  in.tach_ppr = card.tach_ppr;
  in.dt_ms = dt;
}

void flush_storage() {
  if (card.cal_dirty) card_save_calibration();
  if (card.odo.known() && card.odo.fracNano() > 0) card.odo.stampFrac();
}

void step(uint32_t now) {
  uint32_t dt = now - g_last_step;
  if (dt > 250) dt = kStepMs;  // a stall must not add a jump of distance
  g_last_step = now;
  card.steps++;
  card.demo = digitalRead(PIN_DEMO) == HIGH;
  sample_inputs(card.in, dt);
  if (card.demo) {
    card.engine.demo(card.out);
  } else {
    card.engine.step(card.in, card.out);
  }
  if (card.out.ign) g_last_ign_on = now;

  ElcCanFrame f[ELC_CAN_SENSOR_FRAMES];
  legacy_can_frames(card.out, g_seq++, f);
  for (size_t i = 0; i < ELC_CAN_SENSOR_FRAMES; i++) tx_frame(f[i]);
  uint8_t u[kUartFrameLen];
  legacy_uart_frame(card.out, u);
  Serial1.write(u, sizeof(u));

  // an odometer that could not be read is tried again every 5 s (a loose FRAM connector)
  if (!card.odo.known() && now - g_last_odo_retry >= 5000) {
    g_last_odo_retry = now;
    if (card.odo.tryLoad() == OdoLoad::Ok) card.fram = true;
  }
}

void heartbeat(uint32_t now) {
  if (g_last_hb && now - g_last_hb < 1000) return;
  g_last_hb = now ? now : 1;
  ElcCanHeartbeat hb;
  hb.node = kCardNode;
  hb.type = (ElcCanNodeType)ELC_TYPE_SENSOR;
  hb.fw_version = CARD_FW_VERSION;
  hb.uptime_s = now / 1000u;
  hb.flags = 0x01;  // speaks the channel protocol
  ElcCanFrame f;
  elc_can_encode_heartbeat(hb, f);
  tx_frame(f);
}

void power_hold(uint32_t now) {
  static bool was_ign = false;
  const bool ign = card.out.ign;
  if (was_ign && !ign) g_flush_pending = true;  // key-off: write everything before the rail can drop
  was_ign = ign;
  if (g_flush_pending) {
    g_flush_pending = false;
    flush_storage();
  }
  const bool hold = ign || now - g_last_ign_on < kHoldAfterOffMs || card.demo;
  digitalWrite(PIN_PWR_HOLD, hold ? HIGH : LOW);
}

void led(uint32_t now) {
  bool on;
  if (card.identify_until && (int32_t)(card.identify_until - now) > 0) {
    on = (now / 125) & 1;
  } else {
    card.identify_until = 0;
    on = card.center_ms && now - card.center_ms < 1000;  // the center is talking to us
  }
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}
}  // namespace

bool card_save_calibration() {
  if (!card.fram) return false;
  const bool ok = card.store.save(card.cal);
  if (ok) card.cal_dirty = false;
  card.cal_saved_ms = millis();
  return ok;
}

void card_restart() {
  cli();
  wdt_reset();  // the watchdog keeps running across the jump; .init3 stops it again
  TIMSK4 = TIMSK5 = 0;
  ADCSRA = 0;
  Serial.flush();
  asm volatile("jmp 0");  // the Mega's stock bootloader does not clear a watchdog reset, so jump to the start instead
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);  // hold the main board's rail from the start
  pinMode(PIN_IGN, INPUT);
  pinMode(PIN_ALT_L, INPUT);
  pinMode(PIN_TEMP_MON, INPUT);
  pinMode(PIN_PRESS_MON, INPUT);
  pinMode(PIN_DEMO, INPUT);  // the card has its own 10k pull-down
  pinMode(PIN_RPM_SEL_A, INPUT_PULLUP);
  pinMode(PIN_RPM_SEL_B, INPUT_PULLUP);

  elc_usb_begin(ELC_USB_DEFAULT_BAUD);
  Serial1.begin(115200);  // J8: the sensor frames to the main board's J27 (the fallback to the CAN path)
  settings_load(card.set);

  // ---- FRAM: identity, calibration, odometer
  fram_begin();
  delay(20);
  const FramIo io = fram_io();
  card.fram = io.probe(nullptr);
  card.store.begin(io);
  cal::defaults(card.cal);
  card.cal_from_fram = card.fram && card.store.load(card.cal);
  uint32_t uid = 0;
  if (card.fram) {
    if (!identity_read(io, &uid)) {
      uid = card.set.uid ? card.set.uid : make_uid();
      identity_write(io, uid);
    }
  } else if (card.set.uid) {
    uid = card.set.uid;
  } else {
    uid = make_uid();
    card.set.uid = uid;
    settings_save(card.set);
  }
  card.uid = uid;
  card.odo.begin(io, ODO_INITIAL_TENTHS);
  OdoLoad r = OdoLoad::NoChip;
  for (uint8_t tries = 0; tries < (card.fram ? 20 : 3); tries++) {  // a slow FRAM must not be taken for a blank one
    r = card.odo.tryLoad();
    if (r == OdoLoad::Ok) break;
    delay(100);
  }
  note_boot_load(r);
  card.engine.begin(&card.cal, &card.odo);
  card.odo.clearNote();

  adc_begin();
  capture_begin();

  // ---- CAN: the card's MCP2515 (or, on a Mega, the shield's)
  card.can.begin(card.set.can_cs, card.set.can_mhz);
  ElcNetAnnounce me;
  me.type = ELC_TYPE_SENSOR;
  me.product = ELC_PRODUCT_SENSOR_CARD;
  me.fw = CARD_FW_VERSION;
  me.uid = card.uid;
  me.generation = 1;
  me.produces = elc_ch_bit(ELC_CH_SENSOR) | elc_ch_bit(ELC_CH_ALERT) | elc_ch_bit(ELC_CH_UI);
  me.consumes = elc_ch_bit(ELC_CH_SYSTEM) | elc_ch_bit(ELC_CH_UI) | elc_ch_bit(ELC_CH_ALERT);
  me.flags = (card.cal.flags & cal::kFlagDefaults) ? 1 : 0;  // slot 0: the card is node 0x04 in any slot
  strncpy(me.name, "Sensor card", sizeof(me.name) - 1);
  CardNet::Hooks hooks;
  hooks.transfer = on_transfer;
  hooks.describe = on_describe;
  hooks.net_cmd = on_net_cmd;
  card.net.begin(kCardNode, me, tx_frame, tx_free, hooks);
  elc_usb_set_identity(kCardNode, (ElcCanNodeType)ELC_TYPE_SENSOR, CARD_FW_VERSION);
  elc_usb_set_can_running(card.can.running());

  CardUi::Ctx uc;
  uc.cal = &card.cal;
  uc.engine = &card.engine;
  uc.odo = &card.odo;
  uc.net = &card.net;
  uc.out = &card.out;
  uc.uid = card.uid;
  card.ui.begin(uc);
  CardChan::Ctx cc;
  cc.net = &card.net;
  cc.ui = &card.ui;
  cc.out = &card.out;
  cc.cal = &card.cal;
  const uint32_t now = millis();
  card.chan.begin(cc, now);
  g_banner_at = now + kBannerFirstMs;
  g_last_step = now;
  g_last_ign_on = now;  // key-off hold also applies to a boot with the key off
  console_begin();
  // Watchdog from here on: 2 s. setup() above runs without it (the FRAM retries take up to 2 s). Nothing in loop()
  // blocks for long: FRAM writes are a few ms (Wire has a 3 ms timeout), the key-off flush and the WAKE hold are
  // non-blocking, a CAN re-probe is about 12 ms per select line.
  wdt_enable(WDTO_2S);
}

void loop() {
  wdt_reset();
  const uint32_t now = millis();
  elc_usb_pump();

  // frames from the relay (the bench) and from the bus
  ElcCanFrame f;
  int budget = 64;
  while (budget-- > 0 && elc_usb_take_frame(f)) {
    deliver(f, now);
    if (elc_usb_gateway() && card.can.running()) card.can.send(f);  // a laptop on the bus
  }
  card.can.service(now);
  budget = 16;
  while (budget-- > 0 && card.can.receive(f)) {
    if (elc_usb_gateway()) elc_usb_send_frame(f);
    deliver(f, now);
  }
  while (Serial1.available() > 0) {
    if (Serial1.read() == 0xA5) card.center_ms = now ? now : 1;  // the center's keepalive byte on the serial link
  }

  card.net.poll(now);
  card.chan.poll(now);
  card.ui.poll(now);
  heartbeat(now);
  if (now - g_last_step >= kStepMs) step(now);
  uart_banner(now);

  if (card.ui.takeDirty()) card.cal_dirty = true;
  if (card.cal_dirty && card.fram && now - card.cal_saved_ms >= kSaveMinGapMs) card_save_calibration();
  power_hold(now);
  led(now);
  console_poll(now);
  if (card.odo.note()[0]) {
    Serial.println(card.odo.note());
    card.odo.clearNote();
  }
}
