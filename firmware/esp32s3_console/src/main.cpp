// Console board (node 0x13, ConsoleBoardRS): SSD1322 OLED (16 grey levels), two RetroSound knobs (left = Volume,
// right = Select: turn, push, outer ring; the right ring is the two soft keys), CAN. It is the car's radio head unit
// (README.md, Controls);
// logic in console_ui.cpp, drawing in console_screens.cpp (both also run on the host: test/host,
// tools/console_preview). On the channel protocol it announces itself, follows the audio card's sources and shows
// the accessories' pages (accessory_can).

#include <Arduino.h>
#include <accessory_can.h>
#include <elc_usb_link.h>
#include <Preferences.h>
#include <elc_can.h>
#include <elc_fw_esp32.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "console_input.h"
#include "console_pins.h"
#include "console_ui.h"
#include "fw_portal.h"
#include "oled_gfx.h"
#include "oled_ssd1322.h"

#ifndef ELC_CAN_TX_PIN
#define ELC_CAN_TX_PIN -1
#endif
#ifndef ELC_CAN_RX_PIN
#define ELC_CAN_RX_PIN -1
#endif

LV_FONT_DECLARE(con_cl_mid);
LV_FONT_DECLARE(con_cl_body);
LV_FONT_DECLARE(con_cl_small);

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
  s_prefs.putUChar("drive", s_ui.oled_drive());
}

// ---- serial console (bench) ----

static void print_help() {
  Serial.println("commands:");
  Serial.println("  help                 this list");
  Serial.println("  state                mode, page, what the console hears");
  Serial.println("  keys                 what the two soft keys (the right ring) say now");
  Serial.println("  key left|right [hold]    the right ring pushed left / right");
  Serial.println("  left +N|-N|click|hold|ringl|ringr    the left knob (volume, mute, power, source)");
  Serial.println("  right +N|-N|click|hold   the right knob (tune / tracks / lists, OK, menu)");
  Serial.println("  knobs                the knob lines (mV, band, which contact is the encoder)");
  Serial.println("  knobs log on|off     every knob line's level change (mV, band) and each step / click decoded");
  Serial.println("  ir                   the IR receiver: the last remote code heard, the button down");
  Serial.println("  ir log on|off        every IR burst's marks and spaces (us) and what it decoded to");
  Serial.println("  remote KEY [hold]    a remote button: up, down, left, right, select, menu, play, vol+, vol-, mute, band, display, power, 1-9, sel");
  Serial.println("  chan                 the channel node: announced nodes, accessory pages");
  Serial.println("  shot                 the frame as hex (tools/console_preview/shot.py turns it into a PNG)");
  Serial.println("  oled test|off        every pixel on / normal");
  Serial.println("  log on|off           log the frames sent");
  Serial.println("  demo on|off          pretend center, sensors and phone (no bus needed)");
  Serial.println("  demo call            (demo) an incoming call: the right ring right answers, left declines");
  Serial.println("  fw                   firmware: this console's slots, an update in progress, the last upload");
  Serial.println("  fw start|stop        the Wi-Fi update page (as Menu > Service > Firmware update)");
  Serial.println("  fw good              keep the running image now (instead of after 20 s)");
}

// ---- firmware updates (fw_portal.h; this console also takes its own over CAN: elc_fw_esp32.h) ----

/** A frame of the OLED while an update owns it: the Wi-Fi page, or this console taking an image over CAN. */
static bool draw_update(uint32_t now) {
  const ElcFwTarget *t = elc_fw_esp32_target();
  if (fw_portal_active()) {
    fw_portal_draw(s_gfx, now, &con_cl_mid, &con_cl_body, &con_cl_small);
  } else if (t && (t->active() || t->phase() == ElcFwTarget::Phase::Done)) {
    char b[32];
    s_gfx.clear();
    s_gfx.text(t->phase() == ElcFwTarget::Phase::Done ? "Updated, restarting" : "Updating firmware", 4, 28, &con_cl_body, 15);
    snprintf(b, sizeof(b), "%u%%", (unsigned)(t->progress() / 10));
    s_gfx.text(b, 252, 28, &con_cl_body, 15, TextAlign::Right);
    s_gfx.bar(4, 40, 248, 6, t->progress() / 1000.0f, 13, 3);
    s_gfx.text("Keep the ignition on", 4, 60, &con_cl_small, 8);
  } else {
    return false;
  }
  oled_flush(s_gfx, s_ui.level_scale(), s_ui.oled_drive());
  return true;
}

static void portal_redraw(uint32_t now) { draw_update(now); }
static bool portal_stopped() { return s_ui.stopped(millis()); }

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
  {
    char reply[700];
    if (fw_portal_command(line, reply, sizeof(reply))) {  // fw, fw start|stop|good
      Serial.println(reply);
      return;
    }
  }
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
  } else if (!strcmp(cmd, "keys")) {
    ConsoleKey k[ConsoleUi::kKeys];
    s_ui.keys(k, now);
    for (int i = 0; i < ConsoleUi::kKeys; i++) {
      Serial.printf("%s: %s%s\n", i ? "right" : "left", k[i].label[0] ? k[i].label : "-", k[i].hold ? " (has a hold)" : "");
    }
  } else if (!strcmp(cmd, "key") && arg) {
    const char *how = strtok(nullptr, " \t");
    const int i = !strcmp(arg, "right") ? 1 : (!strcmp(arg, "left") ? 0 : -1);
    if (how && !strcmp(how, "hold")) s_ui.key_hold(i, now);
    else s_ui.key_click(i, now);
  } else if (!strcmp(cmd, "knobs") && arg && !strcmp(arg, "log")) {
    const char *how = strtok(nullptr, " \t");
    const bool on = !how || strcmp(how, "off");
    console_input_knob_log(on);
    Serial.printf("knobs log %s\n", on ? "on" : "off");
  } else if (!strcmp(cmd, "knobs")) {
    char buf[320];
    console_input_describe(buf, sizeof(buf));
    Serial.println(buf);
  } else if (!strcmp(cmd, "ir") && arg && !strcmp(arg, "log")) {
    const char *how = strtok(nullptr, " \t");
    const bool on = !how || strcmp(how, "off");
    console_input_ir_log(on);
    Serial.printf("ir log %s\n", on ? "on" : "off");
  } else if (!strcmp(cmd, "ir")) {
    char buf[200];
    console_input_describe_ir(buf, sizeof(buf));
    Serial.println(buf);
  } else if (!strcmp(cmd, "remote") && arg) {
    RemoteKey k = RemoteKey::None;
    for (int i = 1; i < (int)RemoteKey::kCount; i++) {
      if (!strcmp(arg, remote_key_name((RemoteKey)i))) k = (RemoteKey)i;
    }
    const char *how = strtok(nullptr, " \t");
    if (k == RemoteKey::None) Serial.println("remote: up, down, left, right, select, menu, play, vol+, vol-, mute, band, display, power, 1-9");
    else if (how && !strcmp(how, "hold")) s_ui.remote_hold(k, now);
    else if (k == RemoteKey::Up || k == RemoteKey::Down || k == RemoteKey::VolUp || k == RemoteKey::VolDown)
      s_ui.remote_press(k, now);
    else s_ui.remote_click(k, now);
  } else if ((!strcmp(cmd, "left") || !strcmp(cmd, "right")) && arg) {
    const bool left = cmd[0] == 'l';
    if (!strcmp(arg, "click")) left ? s_ui.left_click(now) : s_ui.right_click(now);
    else if (!strcmp(arg, "hold")) left ? s_ui.left_hold(now) : s_ui.right_hold(now);
    else if (left && (!strcmp(arg, "ringl") || !strcmp(arg, "ringr"))) s_ui.left_ring(arg[4] == 'r' ? 1 : -1, now);
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
               (ConsoleContrast)(s_prefs.getUChar("contrast", 0) % 3), s_prefs.getUChar("drive", ConsoleUi::kDriveDefault));
  console_input_begin();
  oled_begin();
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
  // Firmware updates: this console's own image over CAN (a laptop on the bus), and the Wi-Fi page that updates it and
  // the screens (Menu > Service > Firmware update)
  elc_fw_esp32_begin(&s_chan, ELC_PRODUCT_CONSOLE, ELC_CAN_FW_VERSION);
  elc_fw_esp32_set_allowed([](void *) { return s_ui.stopped(millis()); }, nullptr);
  FwPortalConfig pc;
  pc.can = &s_can;
  pc.chan = &s_chan;
  pc.stopped = portal_stopped;
  pc.redraw = portal_redraw;
  fw_portal_begin(pc);
}

void loop() {
  s_can.poll();  // first: frames are stamped with millis() as they arrive, and nothing below may be older than them
  const uint32_t now = millis();

  // A new image is kept once the console has run 20 s (its own Wi-Fi page can always put another on, so running is
  // enough); one that finished arriving over CAN restarts the console here.
  elc_fw_esp32_poll(now, true);
  if (s_ui.take_fw_request() && !fw_portal_start(now)) Serial.println("fw: the update page did not open (see fw)");
  fw_portal_poll(now);

  ConsoleInputEvents ev;
  console_input_poll(ev, now);
  if (fw_portal_active()) {
    // the update page has the knobs: the left soft key or a hold on either knob closes it (not while a file goes)
    const bool back = ev.key_click[0] || ev.key_hold[0] || ev.left_hold || ev.right_hold || ev.left_click;
    if (back && !fw_portal_busy()) fw_portal_stop();
    ev = ConsoleInputEvents();
  }
  if (ev.left_steps) s_ui.left_turn(ev.left_steps, now);
  if (ev.left_click) s_ui.left_click(now);
  if (ev.left_hold) s_ui.left_hold(now);
  if (ev.left_ring) s_ui.left_ring(ev.left_ring, now);
  if (ev.right_steps) s_ui.right_turn(ev.right_steps, now);
  if (ev.right_click) s_ui.right_click(now);
  if (ev.right_hold) s_ui.right_hold(now);
  for (int i = 0; i < 2; i++) {
    if (ev.key_click[i]) s_ui.key_click(i, now);
    if (ev.key_hold[i]) s_ui.key_hold(i, now);
  }
  if (ev.remote.press != RemoteKey::None) s_ui.remote_press(ev.remote.press, now);
  if (ev.remote.step != RemoteKey::None) s_ui.remote_press(ev.remote.step, now);
  if (ev.remote.click != RemoteKey::None) s_ui.remote_click(ev.remote.click, now);
  if (ev.remote.hold != RemoteKey::None) s_ui.remote_hold(ev.remote.hold, now);
  poll_serial();
  accessory_can_poll(now);

  if (s_demo) demo_feed(now);
  s_ui.tick(now);
  send_actions();
  save_persistent();

  static uint32_t s_frame_ms = 0;
  if (now - s_frame_ms >= 33 && !s_all_on) {  // ~30 frames/s: page slides and the marquee stay smooth
    s_frame_ms = now;
    if (!draw_update(now)) {
      s_ui.render(s_gfx, now);
      oled_flush(s_gfx, s_ui.level_scale(), s_ui.oled_drive());
    }
  }
  delay(1);
}
