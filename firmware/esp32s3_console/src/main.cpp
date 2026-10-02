// Console board (node 0x13): SSD1322 OLED (16 grey levels), two knobs (left = Volume encoder, right = Select), two
// soft keys under the OLED (left = Back, right = View), CAN. It is the car's radio head unit (README.md, Controls);
// logic in console_ui.cpp, drawing in console_screens.cpp (both also run on the host: test/host,
// tools/console_preview). On the channel protocol it announces itself, follows the audio card's sources and shows
// the accessories' pages (accessory_can).

#include <Arduino.h>
#include <accessory_can.h>
#include <elc_usb_link.h>
#include <Preferences.h>
#include <elc_can.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "console_input.h"
#include "console_pins.h"
#include "console_ui.h"
#include "oled_gfx.h"
#include "oled_ssd1322.h"

#ifndef ELC_CAN_TX_PIN
#define ELC_CAN_TX_PIN -1
#endif
#ifndef ELC_CAN_RX_PIN
#define ELC_CAN_RX_PIN -1
#endif

static ElcCan s_can;
static ElcChannels s_chan;
static ConsoleUi s_ui(ELC_CAN_NODE_CONSOLE);
static OledGfx s_gfx;
static Preferences s_prefs;
static char s_line[64];
static size_t s_line_n = 0;
static bool s_log_tx = true;
static bool s_all_on = false;

// ---- bench demo: a pretend center, sensor pack and audio bridge, so the pages and menus work with no bus ----

static bool s_demo = false;
static ElcCanClusterState s_demo_cl;
static uint8_t s_demo_call = ELC_CAN_CALL_IDLE;
static bool s_demo_muted = false;
static uint32_t s_demo_call_ms = 0;

/** The demo center answers the console's own setting requests, like the real one. */
static void demo_apply(const ElcCanFrame &f) {
  ElcCanSettingRequest r;
  ElcCanPhoneCommandMsg c;
  if (elc_can_decode_setting_request(f, r)) {
    switch (r.setting) {
      case ELC_CAN_SET_THEME: s_demo_cl.theme = r.value % 3; break;
      case ELC_CAN_SET_CENTER_VIEW: s_demo_cl.center_subview = r.value & 3; break;
      case ELC_CAN_SET_INTERFACE_SCREEN: s_demo_cl.interface_screen = r.value % 3; break;
      case ELC_CAN_SET_MEDIA_SCREEN: s_demo_cl.media_screen = r.value % 3; break;
      case ELC_CAN_SET_BRIGHTNESS: s_demo_cl.brightness = r.value; break;
      case ELC_CAN_SET_DIM_WITH_LIGHTS:
        s_demo_cl.flags = (uint8_t)(r.value ? (s_demo_cl.flags | ELC_CAN_CLUSTER_DIM_WITH_LIGHTS) : (s_demo_cl.flags & ~ELC_CAN_CLUSTER_DIM_WITH_LIGHTS));
        break;
      case ELC_CAN_SET_RIGHT_LAYOUT:
        s_demo_cl.flags = (uint8_t)(r.value ? (s_demo_cl.flags | ELC_CAN_CLUSTER_RIGHT_LAYOUT2) : (s_demo_cl.flags & ~ELC_CAN_CLUSTER_RIGHT_LAYOUT2));
        break;
    }
  } else if (elc_can_decode_phone_command(f, c)) {
    if (c.command == ELC_CAN_PHONE_CMD_ANSWER) { s_demo_call = ELC_CAN_CALL_ACTIVE; s_demo_call_ms = millis(); }
    if (c.command == ELC_CAN_PHONE_CMD_END) s_demo_call = ELC_CAN_CALL_IDLE;
    if (c.command == ELC_CAN_PHONE_CMD_MIC_MUTE) s_demo_muted = c.arg != 0;
  }
}

static void demo_feed(uint32_t now) {
  static uint32_t s_ms = 0;
  if (now - s_ms < 50) return;
  s_ms = now;
  ElcCanFrame f;
  f.id = ELC_CAN_ID_HEARTBEAT_BASE + ELC_CAN_NODE_CENTER;
  f.len = 8;
  s_ui.feed(f, now);
  elc_can_encode_cluster_state(s_demo_cl, f);
  s_ui.feed(f, now);
  // a gentle drive: speed and revs wander, the rest sits in the normal range
  const float w = (float)(now % 20000) / 20000.0f * 6.2832f;
  ElcCanSensor sn;
  sn.speed_rpm = (uint32_t)(575 + 120 * sinf(w));
  sn.engine_rpm = (uint32_t)(2200 + 300 * sinf(w));
  sn.pot_adc = 593;
  sn.pressure_adc = 409;
  sn.therm_adc = 560;
  sn.voltage_adc = 720;
  sn.flags = 0x03;
  sn.odometer_tenths_mile = 966486 + now / 5000;
  sn.dte_tenths_mile = 2750;
  sn.mpg_instant_x10 = (uint16_t)(185 + 20 * sinf(w));
  sn.mpg_average_x10 = 172;
  sn.has_fuel_economy = true;
  ElcCanFrame sf[ELC_CAN_SENSOR_FRAMES];
  elc_can_encode_sensor(sn, sf);
  for (auto &x : sf) s_ui.feed(x, now);
  ElcCanAudioTransport t;
  t.duration_s = 270;
  t.elapsed_s = (uint16_t)((now / 1000) % 270);
  t.volume = s_ui.volume();
  t.connected = true;
  t.text_serial = 1;
  elc_can_encode_audio_transport(0, t, f);
  s_ui.feed(f, now);
  ElcCanFrame tc[ELC_CAN_AUDIO_TEXT_CHUNKS];
  elc_can_encode_audio_text(0, 1, "Fleetwood Mac - The Chain (2004 Remaster)", tc);
  for (auto &x : tc) s_ui.feed(x, now);
  ElcCanTime tm;
  tm.utc = 1790608980u + now / 1000;
  tm.offset_min = -300;
  tm.flags = ELC_CAN_TIME_VALID | ELC_CAN_TIME_GPS;
  elc_can_encode_time(tm, f);
  s_ui.feed(f, now);
  ElcCanPhoneStatus ps;
  ps.flags = (uint8_t)(ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP | ELC_CAN_PHONE_CALL_AUDIO | (s_demo_muted ? ELC_CAN_PHONE_MIC_MUTED : 0));
  ps.call = s_demo_call;
  ps.call_s = s_demo_call == ELC_CAN_CALL_ACTIVE ? (uint16_t)((now - s_demo_call_ms) / 1000) : 0;
  ps.caller_serial = 4;
  ps.call_volume = 11;
  elc_can_encode_phone_status(0, ps, f);
  s_ui.feed(f, now);
  ElcCanFrame cc[ELC_CAN_PHONE_TEXT_CHUNKS];
  elc_can_encode_phone_text(0, ELC_CAN_PHONE_TEXT_CALLER, 4, "(555) 014-2368", cc);
  for (auto &x : cc) s_ui.feed(x, now);
}

static void on_frame(const ElcCanFrame &f, void *) { s_ui.feed(f, millis()); }

static void on_transfer(const ElcChTransfer &t, void *) {
  const uint32_t now = millis();
  accessory_can_transfer(t, now);
  s_ui.transfer(t, now);
}

static void send_actions() {
  ConsoleAction a;
  while (s_ui.pop_action(a)) {
    if (s_demo) demo_apply(a.frame);
    const bool ok = s_can.running() && s_can.send(a.frame);
    if (s_log_tx) Serial.printf("tx 0x%03X %s%s\n", a.frame.id, a.what, ok ? "" : " (not sent)");
  }
  char line[80];
  while (s_ui.pop_log(line, sizeof(line))) {
    if (s_log_tx) Serial.printf("tx %s\n", line);
  }
}

static void save_persistent() {
  if (!s_ui.take_persist_dirty()) return;
  s_prefs.putBool("trip_set", s_ui.trip_set());
  s_prefs.putUInt("trip_base", s_ui.trip_base());
  s_prefs.putUChar("contrast", (uint8_t)s_ui.contrast());
}

// ---- serial console (bench) ----

static void print_help() {
  Serial.println("commands:");
  Serial.println("  help                 this list");
  Serial.println("  state                mode, page, what the console hears");
  Serial.println("  soft left|right [hold]   the soft keys under the OLED (left = Back, right = View)");
  Serial.println("  left +N|-N|click|hold    the left knob (volume, source, menu)");
  Serial.println("  right +N|-N|click|hold   the right knob (tune / tracks, preset / voice, seek / redial)");
  Serial.println("  chan                 the channel node: announced nodes, accessory pages");
  Serial.println("  shot                 the frame as hex (tools/console_preview/shot.py turns it into a PNG)");
  Serial.println("  oled test|off        every pixel on / normal");
  Serial.println("  log on|off           log the frames sent");
  Serial.println("  demo on|off          pretend center, sensors and phone (no bus needed; or hold BACK at power-up)");
  Serial.println("  demo call            (demo) an incoming call: the left soft key declines, the right answers");
}

static void shot() {
  const uint8_t *p = s_gfx.pixels();
  Serial.printf("SHOT %d %d\n", OledGfx::W, OledGfx::H);
  static const char kHex[] = "0123456789abcdef";
  char row[OledGfx::W + 1];
  for (int y = 0; y < OledGfx::H; y++) {
    for (int x = 0; x < OledGfx::W; x++) row[x] = kHex[p[y * OledGfx::W + x] & 15];
    row[OledGfx::W] = '\0';
    Serial.println(row);
  }
  Serial.println("END");
}

static void handle_line(char *line) {
  const uint32_t now = millis();
  char *cmd = strtok(line, " \t");
  char *arg = strtok(nullptr, " \t");
  if (!cmd) return;
  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    print_help();
  } else if (!strcmp(cmd, "state")) {
    char buf[400];
    s_can.describe(buf, sizeof(buf));
    Serial.printf("CAN: %s\n", buf);
    s_ui.describe(buf, sizeof(buf), now);
    Serial.println(buf);
  } else if (!strcmp(cmd, "shot")) {
    shot();
  } else if (!strcmp(cmd, "oled") && arg) {
    s_all_on = !strcmp(arg, "test");
    oled_all_on(s_all_on);
  } else if (!strcmp(cmd, "demo") && arg) {
    if (!strcmp(arg, "call")) {
      s_demo_call = ELC_CAN_CALL_INCOMING;
      s_demo_muted = false;
    } else {
      s_demo = !strcmp(arg, "on");
      s_demo_cl = ElcCanClusterState();
      s_demo_call = ELC_CAN_CALL_IDLE;
    }
    Serial.printf("demo %s\n", s_demo ? "on" : "off");
  } else if (!strcmp(cmd, "log") && arg) {
    s_log_tx = !strcmp(arg, "on");
  } else if (!strcmp(cmd, "soft") && arg) {
    const char *how = strtok(nullptr, " \t");
    const bool hold = how && !strcmp(how, "hold");
    if (!strcmp(arg, "left")) hold ? s_ui.soft_left_hold(now) : s_ui.soft_left(now);
    else hold ? s_ui.soft_right_hold(now) : s_ui.soft_right(now);
  } else if ((!strcmp(cmd, "left") || !strcmp(cmd, "right")) && arg) {
    const bool left = cmd[0] == 'l';
    if (!strcmp(arg, "click")) left ? s_ui.left_click(now) : s_ui.right_click(now);
    else if (!strcmp(arg, "hold")) left ? s_ui.left_hold(now) : s_ui.right_hold(now);
    else {
      const int n = (int)strtol(arg, nullptr, 10);
      left ? s_ui.left_turn(n, now) : s_ui.right_turn(n, now);
    }
  } else if (!strcmp(cmd, "chan")) {
    char buf[600];
    s_chan.describe(buf, sizeof(buf));
    Serial.println(buf);
    accessory_can_describe(buf, sizeof(buf));
    Serial.println(buf);
  } else {
    Serial.printf("unknown: %s (try help)\n", cmd);
  }
}

static void poll_serial() {
  while (elc_usb_available()) {  // the USB link keeps CAN records out of the text
    const int c = elc_usb_read();
    if (c == '\r' || c == '\n') {
      if (s_line_n) {
        s_line[s_line_n] = '\0';
        handle_line(s_line);
        s_line_n = 0;
      }
    } else if (s_line_n < sizeof(s_line) - 1) {
      s_line[s_line_n++] = (char)c;
    }
  }
}

void setup() {
  elc_usb_begin();  // Serial, with the USB CAN link (docs/protocols/usb_link.md)
  s_prefs.begin("console", false);
  s_ui.restore(s_prefs.getBool("trip_set", false), s_prefs.getUInt("trip_base", 0),
               (ConsoleContrast)(s_prefs.getUChar("contrast", 0) % 3));
  console_input_begin();
  // Bench: BACK held at power-up starts the demo (a pretend center, sensors and phone); a normal start leaves it off.
  delay(30);
  if (digitalRead(CONSOLE_BACK_PIN) == LOW) {
    s_demo = true;
    Serial.println("demo on (BACK held at start)");
  }
  oled_begin();
  ledcAttach(CONSOLE_LED_PIN, 5000, 10);
  ledcWrite(CONSOLE_LED_PIN, 0);
  s_can.onFrame(on_frame, nullptr);
  const bool ok = s_can.begin(ELC_CAN_TX_PIN, ELC_CAN_RX_PIN, ELC_CAN_NODE_CONSOLE, ElcCanNodeType::Console);
  Serial.printf("console: node 0x%02X, CAN %s (TX=%d RX=%d). Type help.\n", ELC_CAN_NODE_CONSOLE, ok ? "up" : "off",
                ELC_CAN_TX_PIN, ELC_CAN_RX_PIN);
  // the channel protocol: announce (a console, product 0x0003), the audio card's sources, the accessory pages
  ElcNetAnnounce me;
  me.type = ELC_TYPE_CONSOLE;
  me.product = ELC_PRODUCT_CONSOLE;
  me.fw = ELC_CAN_FW_VERSION;
  me.uid = (uint32_t)(ESP.getEfuseMac() >> 16) ^ (uint32_t)ESP.getEfuseMac();
  me.produces = elc_ch_bit(ELC_CH_AUDIO_CMD) | elc_ch_bit(ELC_CH_UI);  // SOURCE / MEDIA / VOLUME, UI SET / VIEW
  me.consumes = elc_ch_bit(ELC_CH_UI) | elc_ch_bit(ELC_CH_SENSOR) | elc_ch_bit(ELC_CH_SYSTEM);
  snprintf(me.name, sizeof(me.name), "Console");
  s_can.attachChannels(&s_chan, me);
  s_chan.onTransfer(on_transfer, nullptr);
  accessory_can_begin(&s_chan);
  s_ui.attach(&s_chan);
}

void loop() {
  s_can.poll();  // first: frames are stamped with millis() as they arrive, and nothing below may be older than them
  const uint32_t now = millis();

  ConsoleInputEvents ev;
  console_input_poll(ev, now);
  if (ev.left_steps) s_ui.left_turn(ev.left_steps, now);
  if (ev.left_click) s_ui.left_click(now);
  if (ev.left_hold) s_ui.left_hold(now);
  if (ev.right_steps) s_ui.right_turn(ev.right_steps, now);
  if (ev.right_click) s_ui.right_click(now);
  if (ev.right_hold) s_ui.right_hold(now);
  if (ev.soft_left) s_ui.soft_left(now);
  if (ev.soft_left_hold) s_ui.soft_left_hold(now);
  if (ev.soft_right) s_ui.soft_right(now);
  if (ev.soft_right_hold) s_ui.soft_right_hold(now);
  poll_serial();
  accessory_can_poll(now);

  if (s_demo) demo_feed(now);
  s_ui.tick(now);
  send_actions();
  save_persistent();

  static uint32_t s_frame_ms = 0;
  if (now - s_frame_ms >= 33 && !s_all_on) {  // ~30 frames/s: page slides and the marquee stay smooth
    s_frame_ms = now;
    s_ui.render(s_gfx, now);
    oled_flush(s_gfx, s_ui.level_scale());
    ledcWrite(CONSOLE_LED_PIN, (uint32_t)(s_ui.led_level(now) * 1023));
  }
  delay(1);
}
