// Host tests for the console as the car's head unit (console_ui.cpp): the control map (left knob volume / mute /
// power / source, right knob tune or skip / seek / OK, the four stateful soft keys), the OLED menu (display settings
// with live edit and undo, sources, the phone, accessory pages from the UI channel, confirmations), pairing and calls,
// warnings.
// Run: ./run_tests.sh

#include <cstdio>
#include <cstring>
#include <vector>

#include <accessory_can.h>

#include "console_ui.h"

static int g_failures = 0;
#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                 \
    }                                                               \
  } while (0)

static uint32_t T = 10000;

// ---- the bus around the console: its own channel node, and other nodes that talk to it

struct Bus {
  std::vector<ElcCanFrame> frames;
};
static bool capture(const ElcCanFrame &f, void *ctx) {
  ((Bus *)ctx)->frames.push_back(f);
  return true;
}

struct Rig {
  ConsoleUi ui;
  ElcChannels con;   // the console's channel node
  Bus out;           // what the console sent on the channels
  ElcChannels sink;  // a simulated audio card (node 0x14) and radio (0x15)
  ElcChannels radio;
  Bus sink_out, radio_out;

  Rig() {
    ElcNetAnnounce me;
    me.type = ELC_TYPE_CONSOLE;
    me.product = ELC_PRODUCT_CONSOLE;
    std::strcpy(me.name, "Console");
    con.begin(ELC_CAN_NODE_CONSOLE, me, capture, &out);
    con.onTransfer(
        [](const ElcChTransfer &t, void *c) {
          accessory_can_transfer(t, T);
          ((ConsoleUi *)c)->transfer(t, T);
        },
        &ui);
    accessory_can_begin(&con);
    ui.attach(&con);
  }

  /** Frames a node sent, delivered to the console's channel node. */
  void deliver(Bus &b) {
    for (const ElcCanFrame &f : b.frames) con.feed(f, T);
    b.frames.clear();
  }

  void add_sink() {
    ElcNetAnnounce a;
    a.type = ELC_TYPE_AUDIO_SINK;
    a.product = ELC_PRODUCT_AUDIO_CARD;
    a.consumes = elc_ch_bit(ELC_CH_AUDIO_CMD);
    std::strcpy(a.name, "Audio");
    sink.begin(0x14, a, capture, &sink_out);
    sink.poll(0);
    deliver(sink_out);
  }

  void add_radio() {
    ElcNetAnnounce a;
    a.type = ELC_TYPE_RADIO;
    a.product = ELC_PRODUCT_RADIO;
    a.produces = elc_ch_bit(ELC_CH_UI);
    std::strcpy(a.name, "Radio");
    radio.begin(0x15, a, capture, &radio_out);
    radio.poll(0);
    deliver(radio_out);
  }

  void status(uint8_t source, uint8_t volume = 60) {
    ElcChWriter w;
    elc_audio_encode(ElcAudioStatus{volume, 0, 0, 0, 15, source, source == ELC_SRC_RADIO ? (uint8_t)0x15 : (uint8_t)0x14}, w);
    sink.send(ELC_CH_AUDIO_CMD, w);
    deliver(sink_out);
  }

  /** The console's channel messages since the last call, reassembled. */
  std::vector<std::vector<uint8_t>> sent(uint16_t base) {
    std::vector<std::vector<uint8_t>> r;
    ElcChRx rx;
    ElcChTransfer t;
    for (const ElcCanFrame &f : out.frames) {
      if (rx.feed(f, 0, t) && t.base == base) r.emplace_back(t.data, t.data + t.len);
    }
    out.frames.clear();
    return r;
  }
};

static void center(ConsoleUi &ui, uint8_t view = 0) {
  ElcCanFrame f;
  f.id = ELC_CAN_ID_HEARTBEAT_BASE + ELC_CAN_NODE_CENTER;
  f.len = 8;
  ui.feed(f, T);
  ElcCanClusterState c;
  c.center_subview = view;
  elc_can_encode_cluster_state(c, f);
  ui.feed(f, T);
}

static void sensors(ConsoleUi &ui, uint16_t oil_adc = 409) {
  ElcCanSensor s;
  s.engine_rpm = 2200;
  s.pot_adc = 593;
  s.pressure_adc = oil_adc;
  s.therm_adc = 560;
  s.flags = 0x03;
  s.odometer_tenths_mile = 1000;
  ElcCanFrame f[ELC_CAN_SENSOR_FRAMES];
  elc_can_encode_sensor(s, f);
  for (auto &x : f) ui.feed(x, T);
}

/** The console has booted and hears the center and the sensors. */
static void boot(ConsoleUi &ui) {
  ui.tick(T);
  T += ConsoleUi::kBootMs + 300;  // past the power-up bowtie
  center(ui);
  sensors(ui);
  ui.tick(T);
}

static void bridge(ConsoleUi &ui) {
  ElcCanHeartbeat hb;
  hb.node = ELC_CAN_NODE_SLOT0;
  hb.type = ElcCanNodeType::AudioBridge;
  ElcCanFrame f;
  elc_can_encode_heartbeat(hb, f);
  ui.feed(f, T);
}

static void phone(ConsoleUi &ui, uint8_t flags, uint8_t call = ELC_CAN_CALL_IDLE, uint8_t paired = 0) {
  bridge(ui);
  ElcCanPhoneStatus ps;
  ps.flags = flags;
  ps.call = call;
  ps.paired = paired;
  ps.list_serial = 3;
  ps.connected_index = (flags & ELC_CAN_PHONE_CONNECTED) ? 0 : ELC_CAN_PHONE_NONE;
  ElcCanFrame f;
  elc_can_encode_phone_status(0, ps, f);
  ui.feed(f, T);
}

static bool pop(ConsoleUi &ui, ElcCanFrame &f) {
  ConsoleAction a;
  if (!ui.pop_action(a)) return false;
  f = a.frame;
  return true;
}

static void drain(ConsoleUi &ui) {
  ElcCanFrame f;
  while (pop(ui, f)) {
  }
}

/** The next phone command the console queued (command 0 when none). */
static ElcCanPhoneCommandMsg next_phone(ConsoleUi &ui) {
  ElcCanFrame f;
  ElcCanPhoneCommandMsg c;
  c.command = 0;
  while (pop(ui, f)) {
    if (elc_can_decode_phone_command(f, c)) return c;
  }
  c.command = 0;
  return c;
}

/** Select the menu row labelled `label` with the right knob and push it. */
static bool pick(ConsoleUi &ui, const char *label) {
  const ConsoleRow *rows = nullptr;
  const int n = ui.menu_rows(&rows);
  for (int i = 0; i < n; i++) {
    if (!std::strcmp(rows[i].label, label)) {
      ui.right_turn(i - ui.menu_sel(), T);
      ui.right_click(T);
      return true;
    }
  }
  return false;
}

/** The soft key labelled `label` (its index 0..3, top to bottom), -1 when no key says it. */
static int key(ConsoleUi &ui, const char *label) {
  ConsoleKey k[ConsoleUi::kKeys];
  ui.keys(k, T);
  for (int i = 0; i < ConsoleUi::kKeys; i++) {
    if (!std::strcmp(k[i].label, label)) return i;
  }
  return -1;
}

/** Click (or hold) the soft key labelled `label`; false when no key says it. */
static bool press(ConsoleUi &ui, const char *label, bool hold = false) {
  const int i = key(ui, label);
  if (i < 0) return false;
  hold ? ui.key_hold(i, T) : ui.key_click(i, T);
  return true;
}

static const ConsoleRow *row(ConsoleUi &ui, const char *label) {
  const ConsoleRow *rows = nullptr;
  const int n = ui.menu_rows(&rows);
  for (int i = 0; i < n; i++) {
    if (!std::strcmp(rows[i].label, label)) return &rows[i];
  }
  return nullptr;
}

// ---- tests

static void test_drive_pages_and_menu() {
  std::printf("drive pages, menu open / back / exit, display settings live edit and undo\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  boot(ui);
  CHECK(ui.alive(T) && ui.mode() == ConsoleMode::Drive && ui.page() == kPageMedia);
  CHECK(key(ui, "Trip") == 1);  // the right soft key names the next page
  CHECK(press(ui, "Trip") && ui.page() == kPageTrip);
  CHECK(key(ui, "Reset") == 0 && key(ui, "Engine") == 1);  // the trip page's own key on the left
  CHECK(press(ui, "Engine") && press(ui, "Clock") && press(ui, "Media"));
  CHECK(ui.page() == kPageMedia);  // wraps

  ui.right_hold(T);  // the right knob held at rest: the menu
  CHECK(ui.mode() == ConsoleMode::Menu && ui.menu_page() == ConsoleUi::kMHome);
  CHECK(key(ui, "Exit") == 0 && key(ui, "Back") < 0);  // on the top page the left key leaves
  CHECK(press(ui, "Exit") && ui.mode() == ConsoleMode::Drive);
  ui.right_hold(T);
  CHECK(row(ui, "Source") && row(ui, "Phone") && row(ui, "Display") && row(ui, "Service"));
  CHECK(!row(ui, "Radio"));  // no radio card heard
  CHECK(pick(ui, "Display") && ui.menu_page() == ConsoleUi::kMDisplay);
  CHECK(key(ui, "Back") == 0 && key(ui, "Exit") == 1);
  drain(ui);
  CHECK(pick(ui, "Theme") && ui.mode() == ConsoleMode::Edit);
  CHECK(key(ui, "Undo") == 0 && key(ui, "Done") == 1);
  ui.right_turn(1, T);  // live: the setting goes out as it turns
  ElcCanFrame f;
  ElcCanSettingRequest req;
  CHECK(pop(ui, f) && elc_can_decode_setting_request(f, req) && req.setting == ELC_CAN_SET_THEME && req.value == 2);
  CHECK(press(ui, "Undo"));  // puts it back
  CHECK(pop(ui, f) && elc_can_decode_setting_request(f, req) && req.value == 0 && ui.mode() == ConsoleMode::Menu);
  CHECK(pick(ui, "Theme"));
  ui.right_turn(1, T);
  CHECK(press(ui, "Done") && ui.mode() == ConsoleMode::Menu);  // keeps it
  drain(ui);
  CHECK(press(ui, "Back") && ui.menu_page() == ConsoleUi::kMHome);
  CHECK(pick(ui, "Display"));
  CHECK(press(ui, "Exit") && ui.mode() == ConsoleMode::Drive);  // straight out from any depth
  ui.right_hold(T);
  CHECK(pick(ui, "Display"));
  CHECK(press(ui, "Back", true) && ui.mode() == ConsoleMode::Drive);  // Back held: out
  ui.right_hold(T);
  CHECK(pick(ui, "Display"));
  ui.right_hold(T);  // the right knob held again: out
  CHECK(ui.mode() == ConsoleMode::Drive);

  // the left knob is the volume in the menu too, never the selection
  ui.right_hold(T);
  const int sel = ui.menu_sel();
  ui.left_turn(2, T);
  CHECK(ui.menu_sel() == sel && ui.volume() == 2);
}

static void test_sources_and_radio() {
  std::printf("source cycle, radio tuning, presets, seek, mute and store from the controls\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  boot(ui);
  ui.left_hold(T);  // no audio card: nothing to send
  CHECK(r.sent(ELC_CH_AUDIO_CMD).empty());
  r.add_sink();
  r.add_radio();
  r.status(ELC_SRC_BLUETOOTH);
  CHECK(ui.source(T) == ELC_SRC_BLUETOOTH);

  ui.left_hold(T);  // the left knob held: the next source, by name
  auto m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][0] == ELC_AUDIO_SOURCE && m[0][1] == 0x14 && m[0][2] == ELC_SRC_RADIO);
  r.status(ELC_SRC_RADIO);
  CHECK(ui.radio_is_source(T));
  CHECK(key(ui, "Band") == 0 && key(ui, "Trip") == 1);
  ui.left_hold(T);  // then off ...
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][2] == ELC_SRC_REQ_OFF);
  r.status(ELC_SRC_OFF);
  ui.left_hold(T);  // ... and round to Bluetooth
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][2] == ELC_SRC_BLUETOOTH);
  r.status(ELC_SRC_RADIO);

  ui.right_turn(2, T);  // right knob: tune, one step per detent
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 2 && m[0][0] == ELC_AUDIO_MEDIA && m[0][1] == 0x14 && m[0][2] == ELC_MEDIA_TUNE_UP);
  ui.right_turn(-1, T);
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][2] == ELC_MEDIA_TUNE_DOWN);
  // the preset list: presets 1 and 3 stored (the radio's items 15..26, kHz)
  for (const ElcUiValue &pv : {ElcUiValue{15, 94700}, ElcUiValue{17, 1070}}) {
    ElcUiItem it;
    it.item = pv.item;
    it.kind = ELC_UI_NUMBER;
    it.parent = 14;
    std::snprintf(it.label, sizeof(it.label), "Preset %d", pv.item - 14);
    ElcChWriter iw, vw;
    elc_ui_encode(it, iw);
    elc_ui_encode(pv, vw);
    r.radio.send(ELC_CH_UI, iw);
    r.radio.send(ELC_CH_UI, vw);
  }
  r.deliver(r.radio_out);
  uint32_t khz = 0;
  CHECK(ui.preset_khz(0, &khz) && khz == 94700 && !ui.preset_khz(1, nullptr) && ui.preset_khz(2, &khz) && khz == 1070);
  ui.right_click(T);  // the right knob push with the radio playing: the preset list, on slot 1
  CHECK(ui.presets_shown() && ui.preset_sel() == 0 && r.sent(ELC_CH_AUDIO_CMD).empty());
  CHECK(key(ui, "Close") == 0 && key(ui, "Store") == 1);
  ui.right_turn(1, T);  // slot 2 is empty: nothing to tune
  CHECK(ui.preset_sel() == 1 && r.sent(ELC_CH_AUDIO_CMD).empty());
  CHECK(press(ui, "Store"));  // a click only says to hold it
  CHECK(r.sent(ELC_CH_AUDIO_CMD).empty());
  CHECK(press(ui, "Store", true));  // held: store the station in slot 2
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][2] == ELC_MEDIA_PRESET_STORE && m[0][3] == 2);
  ui.right_turn(1, T);  // slot 3 holds 1070 AM: only the highlight moves
  CHECK(ui.preset_sel() == 2 && r.sent(ELC_CH_AUDIO_CMD).empty());
  ui.right_turn(-4, T);  // wraps to slot 11
  CHECK(ui.preset_sel() == 10);
  ui.right_turn(4, T);  // back on slot 3
  ui.right_click(T);  // the push tunes to it and closes the list
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(!ui.presets_shown() && m.size() == 1 && m[0][2] == ELC_MEDIA_PRESET_RECALL && m[0][3] == 3);
  CHECK(ui.mode() == ConsoleMode::Drive);
  ui.right_click(T);
  CHECK(ui.presets_shown() && ui.preset_sel() == 0);  // reopens on slot 1 (no preset reported playing)
  CHECK(press(ui, "Close") && !ui.presets_shown() && r.sent(ELC_CH_AUDIO_CMD).empty());  // Close does not tune
  ui.right_click(T + 5);  // an input stamped just after the clock of the next pass (the serial console) ...
  ui.tick(T);
  CHECK(ui.presets_shown());  // ... does not count as long untouched
  T += ConsoleUi::kPresetsIdleMs + 100;
  center(ui);  // the center and the audio card are still there
  sensors(ui);
  r.status(ELC_SRC_RADIO);
  ui.tick(T);
  CHECK(!ui.presets_shown());  // closes by itself when left alone ...
  CHECK(r.sent(ELC_CH_AUDIO_CMD).empty());  // ... without changing the station
  ui.left_ring(1, T);  // the left ring: seek
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][2] == ELC_MEDIA_SEEK_UP);
  ui.left_ring(-1, T);
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][2] == ELC_MEDIA_SEEK_DOWN);
  ui.left_click(T);  // the left knob push: mute
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][2] == ELC_MEDIA_TOGGLE);
  drain(ui);
  ElcCanFrame f;
  CHECK(!pop(ui, f));  // nothing went to the Bluetooth bridge while the radio plays

  // NOW PLAYING from the radio shows on the media page
  ElcAudioNow np;
  np.source = ELC_SRC_RADIO;
  np.preset = 2;
  std::strcpy(np.line1, "94.7 FM  THE WAVE");
  std::strcpy(np.line2, "Now: Sea Breeze");
  ElcChWriter w;
  elc_audio_encode(np, w);
  r.radio.send(ELC_CH_AUDIO_CMD, w);
  r.deliver(r.radio_out);
  char d[600];
  ui.describe(d, sizeof(d), T);
  CHECK(std::strstr(d, "94.7 FM  THE WAVE") != nullptr);

  // off (from the menu's Source page here): tuning and seeking do nothing, and a push puts the radio back
  r.status(ELC_SRC_OFF);
  CHECK(ui.audio_off(T) && key(ui, "Audio on") == 0);
  r.sent(ELC_CH_AUDIO_CMD);
  ui.right_turn(1, T);
  ui.left_ring(1, T);
  ui.right_click(T);
  CHECK(r.sent(ELC_CH_AUDIO_CMD).empty() && !ui.presets_shown());
  ui.left_click(T);
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][0] == ELC_AUDIO_SOURCE && m[0][2] == ELC_SRC_RADIO);
  r.status(ELC_SRC_RADIO);
  r.deliver(r.radio_out);

  // the Source page picks one directly
  ui.right_hold(T);
  CHECK(row(ui, "Radio") && pick(ui, "Source") && ui.menu_page() == ConsoleUi::kMSource);
  CHECK(row(ui, "Radio") && !std::strcmp(row(ui, "Radio")->value, "Playing"));
  CHECK(row(ui, "Off") != nullptr);
  CHECK(pick(ui, "Bluetooth"));
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][0] == ELC_AUDIO_SOURCE && m[0][2] == ELC_SRC_BLUETOOTH && ui.mode() == ConsoleMode::Drive);

  // volume: the left knob, to the bridge (0..15) and the audio card (0..100)
  r.status(ELC_SRC_BLUETOOTH, 60);
  phone(ui, ELC_CAN_PHONE_CONNECTED);
  drain(ui);
  ui.left_turn(1, T);
  ElcCanPhoneCommandMsg c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_MEDIA_VOLUME && c.arg == 10);
  m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][0] == ELC_AUDIO_VOLUME && m[0][2] == 0 && m[0][3] == 67);
}

static void transport(ConsoleUi &ui, uint16_t elapsed, bool playing = true) {
  bridge(ui);
  ElcCanAudioTransport t;
  t.connected = true;
  t.playing = playing;
  t.elapsed_s = elapsed;
  t.duration_s = 200;
  ElcCanFrame f;
  elc_can_encode_audio_transport(0, t, f);
  ui.feed(f, T);
}

static void test_bluetooth_follows_source() {
  std::printf("Bluetooth on the bridge pauses when the source leaves it and plays when it comes back\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  boot(ui);
  r.add_sink();
  r.add_radio();
  r.status(ELC_SRC_BLUETOOTH);
  transport(ui, 10);  // playing: the elapsed time moves
  T += 1000;
  transport(ui, 11);
  center(ui);
  drain(ui);
  r.status(ELC_SRC_RADIO);
  ElcCanPhoneCommandMsg c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_MEDIA && c.arg == 4);
  r.status(ELC_SRC_BLUETOOTH);
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_MEDIA && c.arg == 3);
  // paused before leaving: it stays paused on return
  T += 5000;
  center(ui);
  r.status(ELC_SRC_BLUETOOTH);  // the audio card's 1 Hz STATUS
  transport(ui, 11, false);
  drain(ui);
  r.status(ELC_SRC_RADIO);
  CHECK(next_phone(ui).arg == 4);
  r.status(ELC_SRC_BLUETOOTH);
  CHECK(next_phone(ui).command == 0);
}

static void test_bluetooth_controls() {
  std::printf("Bluetooth: tracks, play / pause, redial, voice assistant, the center view\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  boot(ui);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP);
  drain(ui);
  ui.right_turn(1, T);
  CHECK(next_phone(ui).command == ELC_CAN_PHONE_CMD_MEDIA);
  ui.right_turn(-1, T);
  ElcCanPhoneCommandMsg c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_MEDIA && c.arg == 2);
  ui.left_ring(1, T);  // the left ring skips tracks
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_MEDIA && c.arg == 1);
  ui.left_ring(-1, T);
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_MEDIA && c.arg == 2);
  CHECK(key(ui, "Redial") == 0 && key(ui, "Trip") == 1);  // a phone for calls: Redial (play / pause is the push)
  CHECK(press(ui, "Redial"));
  CHECK(next_phone(ui).command == ELC_CAN_PHONE_CMD_REDIAL);
  ui.left_click(T);  // the left knob push plays / pauses
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_MEDIA && c.arg == 0);
  phone(ui, ELC_CAN_PHONE_CONNECTED);  // audio only (no hands-free): the phone page instead
  CHECK(key(ui, "Phone") == 0 && key(ui, "Redial") < 0);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP);
  ui.right_click(T);  // the right knob push with Bluetooth playing: the voice assistant
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_VOICE && c.arg == 1 && !ui.presets_shown());
  CHECK(press(ui, "Trip", true));  // the page key held: the next center view
  ElcCanFrame f;
  ElcCanSettingRequest req;
  CHECK(pop(ui, f) && elc_can_decode_setting_request(f, req) && req.setting == ELC_CAN_SET_CENTER_VIEW);
}

static void test_phone_menu_and_pairing() {
  std::printf("phone page: pair, paired list, connect, forget with a confirmation; the pairing code\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  boot(ui);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP, ELC_CAN_CALL_IDLE, 2);
  ElcCanFrame tf[ELC_CAN_PHONE_TEXT_CHUNKS];
  elc_can_encode_phone_text(0, ELC_CAN_PHONE_TEXT_PAIRED + 1, 3, "Rob's iPad", tf);
  for (auto &x : tf) ui.feed(x, T);
  CHECK(!std::strcmp(ui.phone_paired_name(1), "Rob's iPad"));
  drain(ui);
  ui.right_hold(T);
  CHECK(pick(ui, "Phone") && ui.menu_page() == ConsoleUi::kMPhone);
  CHECK(pick(ui, "Pair new phone"));
  ElcCanPhoneCommandMsg c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_PAIRING && c.arg == 1);
  CHECK(row(ui, "Phone 1") && !std::strcmp(row(ui, "Phone 1")->value, "Connected"));
  CHECK(pick(ui, "Rob's iPad") && ui.menu_page() == ConsoleUi::kMPhoneOne);
  CHECK(pick(ui, "Connect"));
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_CONNECT && c.arg == 1 && c.list_serial == 3);
  CHECK(pick(ui, "Rob's iPad") && pick(ui, "Forget") && ui.menu_page() == ConsoleUi::kMConfirm);
  CHECK(ui.menu_sel() == 1);  // Cancel first
  CHECK(key(ui, "No") == 0 && key(ui, "Yes") == 1);
  CHECK(press(ui, "No") && ui.menu_page() == ConsoleUi::kMPhoneOne);
  CHECK(pick(ui, "Forget"));
  drain(ui);
  CHECK(press(ui, "Yes"));
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_FORGET && c.arg == 1 && ui.menu_page() == ConsoleUi::kMPhone);

  // a phone asks to pair: the code takes over, the soft keys reject / pair
  phone(ui, ELC_CAN_PHONE_PAIRING | ELC_CAN_PHONE_CONFIRM);
  ElcCanFrame pf;
  elc_can_encode_phone_pairing(0, 123456, 90, pf);
  ui.feed(pf, T);
  CHECK(ui.pair_shown(T));
  CHECK(key(ui, "Reject") == 0 && key(ui, "Pair") == 1);
  drain(ui);
  ui.right_click(T);  // the knob does not accept a code by accident
  CHECK(next_phone(ui).command == 0);
  CHECK(press(ui, "Pair"));
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_CONFIRM && c.arg == 1);
  CHECK(press(ui, "Reject"));
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_CONFIRM && c.arg == 0);
}

static void test_accessory_pages() {
  std::printf("accessory pages from the UI channel: the radio's page, a number editor, a choice, a confirmation\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  boot(ui);
  r.add_sink();
  r.add_radio();
  ElcUiItem on;
  on.item = 0;
  on.kind = ELC_UI_TOGGLE;
  std::strcpy(on.label, "Radio on");
  ElcUiItem fq;
  fq.item = 2;
  fq.kind = ELC_UI_NUMBER;
  fq.exponent = -1;
  fq.min = 875;
  fq.max = 1080;
  fq.step = 1;
  std::strcpy(fq.label, "Frequency");
  std::strcpy(fq.unit, "MHz");
  ElcUiItem st;
  st.item = 10;
  st.kind = ELC_UI_CHOICE;
  st.flags = ELC_UI_F_CONFIRM;
  std::strcpy(st.label, "Store as preset");
  std::strcpy(st.options, "1  --\n2  --");
  for (const ElcUiItem *it : {&on, &fq, &st}) {
    ElcChWriter w;
    elc_ui_encode(*it, w);
    r.radio.send(ELC_CH_UI, w);
  }
  ElcChWriter v1, v2;
  elc_ui_encode(ElcUiValue{0, 1}, v1);
  elc_ui_encode(ElcUiValue{2, 947}, v2);
  r.radio.send(ELC_CH_UI, v1);
  r.radio.send(ELC_CH_UI, v2);
  r.deliver(r.radio_out);
  r.sent(ELC_CH_UI);

  ui.right_hold(T);
  CHECK(pick(ui, "Radio") && ui.menu_page() == ConsoleUi::kMAccPage);
  CHECK(row(ui, "Frequency") && !std::strcmp(row(ui, "Frequency")->value, "94.7 MHz"));
  CHECK(row(ui, "Radio on") && !std::strcmp(row(ui, "Radio on")->value, "On"));
  CHECK(pick(ui, "Frequency") && ui.mode() == ConsoleMode::Edit);
  ui.right_turn(3, T);  // the right knob changes values in an editor
  ui.right_click(T);    // and its push sends it
  auto m = r.sent(ELC_CH_UI);
  bool set = false;
  for (auto &p : m) set |= p[0] == ELC_UI_SET && p[1] == 0x15 && p[2] == 2 && p[3] == 0xB6 && p[4] == 0x03;  // 950
  CHECK(set && ui.mode() == ConsoleMode::Menu);
  CHECK(pick(ui, "Radio on"));  // a toggle: SET 0 at once
  m = r.sent(ELC_CH_UI);
  set = false;
  for (auto &p : m) set |= p[0] == ELC_UI_SET && p[2] == 0 && p[3] == 0;
  CHECK(set);
  CHECK(pick(ui, "Store as preset") && ui.menu_page() == ConsoleUi::kMAccChoice);
  CHECK(pick(ui, "2  --") && ui.menu_page() == ConsoleUi::kMConfirm);  // the item asks to confirm
  r.sent(ELC_CH_UI);
  CHECK(press(ui, "No") && ui.menu_page() == ConsoleUi::kMAccPage && r.sent(ELC_CH_UI).empty());
  CHECK(pick(ui, "Store as preset") && pick(ui, "2  --"));
  CHECK(pick(ui, "OK"));
  m = r.sent(ELC_CH_UI);
  set = false;
  for (auto &p : m) set |= p[0] == ELC_UI_SET && p[2] == 10 && p[3] == 1;
  CHECK(set && ui.menu_page() == ConsoleUi::kMAccPage);
}

static void test_alerts() {
  std::printf("warnings: shown at rest, acknowledged with the OK key, re-armed when clear\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  boot(ui);
  sensors(ui, 50);  // low oil pressure at 2200 rpm
  const ConsoleAlert *a = ui.alert(T);
  CHECK(a && !std::strcmp(a->id, "oil"));
  CHECK(key(ui, "OK") == 1 && key(ui, "Trip") < 0);
  CHECK(press(ui, "OK"));
  CHECK(!ui.alert(T));
  sensors(ui);  // clears
  CHECK(!ui.alert(T));
  sensors(ui, 50);
  CHECK(ui.alert(T) != nullptr);
}

static void test_calls() {
  std::printf("calls: the ring answers / declines / mutes / ends, the push moves the audio, the left knob is the call volume\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  boot(ui);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP, ELC_CAN_CALL_INCOMING);
  drain(ui);
  CHECK(ui.call_shown(T));
  CHECK(key(ui, "Decline") == 0 && key(ui, "Answer") == 1);
  CHECK(press(ui, "Answer"));
  CHECK(next_phone(ui).command == ELC_CAN_PHONE_CMD_ANSWER);
  ui.right_click(T);  // the right knob push answers too
  CHECK(next_phone(ui).command == ELC_CAN_PHONE_CMD_ANSWER);
  CHECK(press(ui, "Decline"));
  CHECK(next_phone(ui).command == ELC_CAN_PHONE_CMD_END);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP | ELC_CAN_PHONE_CALL_AUDIO, ELC_CAN_CALL_ACTIVE);
  drain(ui);
  CHECK(key(ui, "End") == 0 && key(ui, "Mute") == 1);
  CHECK(press(ui, "Mute"));
  ElcCanPhoneCommandMsg c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_MIC_MUTE && c.arg == 1);
  ui.right_click(T);  // the push in a call: its audio to the phone
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_AUDIO_ROUTE && c.arg == 0);
  ui.left_turn(1, T);
  CHECK(next_phone(ui).command == ELC_CAN_PHONE_CMD_CALL_VOLUME);
  ui.right_hold(T);  // no menu, no source change in a call
  ui.left_ring(1, T);
  ui.left_hold(T);
  CHECK(ui.mode() == ConsoleMode::Drive && next_phone(ui).command == 0);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP | ELC_CAN_PHONE_MIC_MUTED, ELC_CAN_CALL_WAITING);
  drain(ui);
  CHECK(key(ui, "Ignore") == 0 && key(ui, "Answer") == 1);
  CHECK(press(ui, "Answer"));  // holds the active call
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_CALL_WAITING && c.arg == 2);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP | ELC_CAN_PHONE_MIC_MUTED, ELC_CAN_CALL_HELD);
  CHECK(key(ui, "End") == 0 && key(ui, "Swap") == 1);
  ui.left_click(T);  // the left knob push: the microphone
  c = next_phone(ui);
  CHECK(c.command == ELC_CAN_PHONE_CMD_MIC_MUTE && c.arg == 0);
}

static void test_call_keeps_source() {
  std::printf("a call: the source it found comes back when it ends\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  boot(ui);
  r.add_sink();
  r.add_radio();
  r.status(ELC_SRC_OFF);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP, ELC_CAN_CALL_INCOMING);
  ui.tick(T);
  r.status(ELC_SRC_RADIO);  // something changed it during the call
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP, ELC_CAN_CALL_ACTIVE);
  ui.tick(T);
  r.sent(ELC_CH_AUDIO_CMD);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP, ELC_CAN_CALL_IDLE);
  ui.tick(T);
  auto m = r.sent(ELC_CH_AUDIO_CMD);
  CHECK(m.size() == 1 && m[0][0] == ELC_AUDIO_SOURCE && m[0][2] == ELC_SRC_REQ_OFF);
  // unchanged during the call: nothing to send
  r.status(ELC_SRC_BLUETOOTH);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP, ELC_CAN_CALL_ACTIVE);
  ui.tick(T);
  phone(ui, ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP, ELC_CAN_CALL_IDLE);
  ui.tick(T);
  CHECK(r.sent(ELC_CH_AUDIO_CMD).empty());
}

static void test_trip_reset() {
  std::printf("trip page: Reset is a held key, a click only says so\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  boot(ui);
  CHECK(press(ui, "Trip") && key(ui, "Reset") == 0);
  CHECK(press(ui, "Reset") && !ui.trip_set());
  CHECK(press(ui, "Reset", true) && ui.trip_set() && ui.trip_base() == 1000 && ui.take_persist_dirty());
}

static void test_no_center() {
  std::printf("no center: the controls do nothing\n");
  Rig r;
  ConsoleUi &ui = r.ui;
  ui.tick(T);
  T += ConsoleUi::kBootMs + 300;
  ui.tick(T);
  CHECK(!ui.alive(T));
  ConsoleKey k[ConsoleUi::kKeys];
  ui.keys(k, T);
  for (auto &x : k) CHECK(!x.label[0]);
  ui.right_hold(T);
  ui.key_click(1, T);
  ui.left_turn(3, T);
  ui.left_ring(1, T);
  CHECK(ui.mode() == ConsoleMode::Drive && ui.page() == kPageMedia);
  ElcCanFrame f;
  CHECK(!pop(ui, f));
}

int main() {
  test_drive_pages_and_menu();
  test_sources_and_radio();
  test_bluetooth_controls();
  test_bluetooth_follows_source();
  test_phone_menu_and_pairing();
  test_accessory_pages();
  test_alerts();
  test_calls();
  test_call_keeps_source();
  test_trip_reset();
  test_no_center();
  if (g_failures) {
    std::printf("%d FAILED\n", g_failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
