#pragma once

// The console is the car's radio head unit (decided 2026-10-02; the El Camino Cluster prototype's drive pages and
// warnings stay). The only controls are two RetroSound knobs (ConsoleBoardRS), each a turn, a push and an outer ring
// that pushes left or right: the left knob is the audio (volume, mute, source, seek / skip) everywhere; the right knob tunes
// at rest and moves through lists, its push is OK (the menu at rest) and its hold the voice assistant, and its ring is two soft keys whose labels
// sit in the screen's bottom corners and change with what is on the screen. Its own menu on the OLED holds Source,
// Radio and the other accessory pages (UI channel), Phone, Display, Trip and Service. No Arduino dependency: it takes
// CAN frames, channel transfers and knob input with a clock, and queues the frames to send, so it runs in the host
// tests and the host preview. Drawing: console_screens.cpp. Controls: firmware/esp32s3_console/README.md.

#include <stddef.h>
#include <stdint.h>

#include <accessory_can.h>
#include <elc_can_codec.h>
#include <elc_chan.h>
#include <elc_chan_msg.h>

#include "ir_remote.h"

class OledGfx;
struct _lv_font_t;
typedef struct _lv_font_t lv_font_t;

/** A frame the console wants on the bus. */
struct ConsoleAction {
  ElcCanFrame frame;
  char what[48];  // for the serial log
};

enum class ConsoleMode : uint8_t { Drive, Menu, Edit };
enum ConsolePage : uint8_t { kPageMedia, kPageTrip, kPageEngine, kPageClock, kPageCount };
enum class ConsoleContrast : uint8_t { Auto, Bright, Night };

/** Readings from the sensor frames the center re-sends (0x100-0x104), in gauge units. */
struct ConsoleSensors {
  bool fresh = false;
  float mph = 0, rpm = 0, fuel = 0, coolant = 100, oil = 0, amp = 0;
  int8_t charging = -1;  // 1 charging, 0 not (warning), -1 key off / unknown
  bool odo_valid = false;
  uint32_t odo_tenths = 0;
  bool econ = false;
  float avg = 0, inst = 0, dte = 0;
};

struct ConsoleAlert {
  const char *id;
  const char *title;
  char line[64];
};

/** A soft key (the right ring pushed left or right) as the screen shows it: its label, and what a click and a hold do
 *  (ConsoleUi::KeyAct). A key with only a hold action (storing a preset, resetting the trip) answers a click with a
 *  hint. */
struct ConsoleKey {
  uint8_t click = 0, hold = 0;
  char label[10] = {0};
};

/** One row of a menu page. */
struct ConsoleRow {
  enum Kind : uint8_t { Sub, Action, Setting, Info };
  uint8_t kind = Info;
  uint16_t id = 0;   // what it does (ConsoleUi::RowId)
  uint8_t arg = 0;   // a paired phone, an accessory item, an option
  char label[33] = {0};
  char value[33] = {0};
  bool dim = false;  // greyed (read-only, not available)
};

class ConsoleUi {
 public:
  explicit ConsoleUi(uint8_t node = ELC_CAN_NODE_CONSOLE) : _node(node) {}

  /** The channel node the console sends through (AUDIO_CMD, UI); null = not on the channel protocol. */
  void attach(ElcChannels *ch) { _ch = ch; }

  /** Persistent settings kept by the console (NVS in the firmware). */
  void restore(bool trip_set, uint32_t trip_base_tenths, ConsoleContrast contrast);
  bool trip_set() const { return _trip_set; }
  uint32_t trip_base() const { return _trip_base; }
  ConsoleContrast contrast() const { return _contrast; }
  /** True once after a persistent setting changed (the firmware saves them). */
  bool take_persist_dirty();

  // ---- input: left knob = Volume, right knob = Select (turn in detents, + = clockwise; push click / hold). The left
  // ring flicked left (-1) or right (+1); the right ring is the two soft keys, 0 = left, 1 = right (click / hold)
  void feed(const ElcCanFrame &f, uint32_t now_ms);
  /** Every complete channel transfer (also give it to accessory_can_transfer, which the firmware does). */
  void transfer(const ElcChTransfer &t, uint32_t now_ms);
  void left_turn(int steps, uint32_t now_ms);
  void left_click(uint32_t now_ms);
  void left_hold(uint32_t now_ms);
  void left_ring(int dir, uint32_t now_ms);
  void right_turn(int steps, uint32_t now_ms);
  void right_click(uint32_t now_ms);
  void right_hold(uint32_t now_ms);
  void key_click(int key, uint32_t now_ms);
  void key_hold(int key, uint32_t now_ms);
  // An IR remote (Apple, Sirius), on top of the knobs: Up / Down are the right knob's turn (the highlight in a list,
  // up the list; at rest and in an editor up = clockwise), Select its push, Left / Right the soft keys, Menu the menu
  // at rest and Back (the left soft key) in it; Vol + / - the left knob's turn, Play and Mute its push, Band its hold
  // (the next source), Display the next drive page (held: the next center view), Power the audio off (off: the source it had back),
  // the digits 1-9 that radio preset. A press is Up / Down / Vol going down or
  // auto-repeating.
  void remote_press(RemoteKey k, uint32_t now_ms);
  void remote_click(RemoteKey k, uint32_t now_ms);
  void remote_hold(RemoteKey k, uint32_t now_ms);
  void tick(uint32_t now_ms);

  static constexpr int kKeys = 2;
  /** What the two soft keys are now, left then right (an empty label = the key does nothing). */
  void keys(ConsoleKey out[kKeys], uint32_t now_ms);

  bool pop_action(ConsoleAction &out);
  /** A line for the serial log about a channel message the console sent ("" when none waits). */
  bool pop_log(char *out, size_t n);

  // ---- output
  /** Draws the whole screen for time now_ms. */
  void render(OledGfx &g, uint32_t now_ms);
  /** 0..1 panel brightness for the grey levels (OLED contrast setting and the cluster brightness). */
  float level_scale() const;
  /** One line per state, for the serial console. */
  void describe(char *out, size_t n, uint32_t now_ms) const;

  // ---- state (tests, serial console)
  ConsoleMode mode() const { return _mode; }
  uint8_t page() const { return _page; }
  bool alive(uint32_t now_ms) const;
  const ConsoleSensors &sensors() const { return _s; }
  const ElcCanClusterState &cluster() const { return _cl; }
  const ElcCanPhoneStatus &phone() const { return _ph; }
  bool phone_fresh(uint32_t now_ms) const { return _ph_ms && now_ms - _ph_ms < 3000; }
  bool call_shown(uint32_t now_ms) const;
  bool pair_shown(uint32_t now_ms) const;
  const ConsoleAlert *alert(uint32_t now_ms);  // the unacknowledged warning shown, or null
  uint8_t bridge_slot() const { return _slot; }
  uint8_t volume() const { return _vol; }
  /** The source the audio card plays (ELC_SRC_*), ELC_SRC_NONE with no audio card. */
  uint8_t source(uint32_t now_ms) const;
  bool radio_is_source(uint32_t now_ms) const { return source(now_ms) == ELC_SRC_RADIO; }
  bool aux_is_source(uint32_t now_ms) const { return source(now_ms) == ELC_SRC_AUX; }
  /** The audio card is the Bluetooth bridge too (one board, one node): AUDIO_CMD covers Bluetooth, so the old phone
   *  frames must not repeat a play / pause, a skip or the volume. */
  bool sink_is_bridge() const;
  /** A band's gain (ELC_EQ_BANDS) on the audio card's Equalizer as the console last heard it; false before it has. */
  bool eq_band(int band, int *db) const;
  /** The audio card says no source plays (off). With no audio card heard this is false: the bridge's Bluetooth runs. */
  bool audio_off(uint32_t now_ms) const { return sink() && _st_ms && now_ms - _st_ms <= 3000 && _st.source == ELC_SRC_OFF; }
  /** The radio's preset list is up (right knob click with the radio playing), and the slot highlighted (0..11). */
  bool presets_shown() const { return _pre_open; }
  int preset_sel() const { return _pre_sel; }
  /** Preset i's frequency in kHz from the radio's preset items (radio_card.md, items 15-26); false when empty. */
  bool preset_khz(int i, uint32_t *khz) const;
  /** The menu page shown and its rows (tests, describe). */
  uint8_t menu_page() const { return _stack[_depth].page; }
  int menu_rows(const ConsoleRow **rows);
  int menu_sel() const { return _stack[_depth].sel; }
  const char *phone_paired_name(uint8_t i) const;
  /** Gallons in a full tank: the sensor card's Tank capacity, else 20. */
  static float tank_gal() { return accessory_can_tank_gal(20.0f); }

  static constexpr uint32_t kCenterGoneMs = 2500;  // the center's heartbeat is 1 Hz: a couple missed before the bowtie
  static constexpr uint32_t kBootMs = 1700;        // bowtie at power-up: fade in, then a sheen
  static constexpr uint32_t kToastMs = 1800;
  static constexpr uint32_t kVolumeShowMs = 1450;
  static constexpr uint32_t kPresetsIdleMs = 4000;  // the preset list closes after this long untouched
  static constexpr int kPresets = 12;
  static constexpr uint8_t kRadioPresetItem0 = 15;  // the radio's preset frequency items 15..26 (radio_card.md)
  static constexpr uint32_t kHintMs = 2600;        // soft-key labels stay lit this long after a control is touched

  // Menu pages.
  enum MenuPage : uint8_t { kMHome, kMSource, kMPhone, kMPhoneOne, kMDisplay, kMTrip, kMService, kMAccessories,
                            kMAccPage, kMAccChoice, kMConfirm, kMEq };
  // What a row does.
  enum RowId : uint16_t {
    kRNone, kRSource, kRRadio, kRPhone, kRAccessories, kRDisplay, kRTrip, kRService,  // home
    kRSrcPick,                                                                          // source page (arg = code)
    kRPhStatus, kRPhPair, kRPhStop, kRPhVoice, kRPhRedial, kRPhPaired,                  // phone page (arg = index)
    kRPhConnect, kRPhDisconnect, kRPhForget,                                            // one phone
    kRView, kRTheme, kRBright, kRFollow, kRIface, kRMedia, kRLayout, kRContrast,        // display settings
    kRTripReset,
    kRAccNode, kRAccItem, kRAccOpt, kRAdvanced,                                         // accessories (arg)
    kRYes, kRNo,                                                                        // confirmation
    kRSound, kREqBand,                                                                  // the sink's page, a band (arg)
  };
  // What a soft key does.
  enum KeyAct : uint8_t {
    kKNone,
    kKAnswer, kKDecline, kKEnd, kKMicMute, kKAudioRoute, kKSwap, kKIgnore,  // calls
    kKPair, kKReject,                                                        // a pairing code
    kKAlertOk,                                                               // a warning
    kKBack, kKExit, kKDone, kKUndo, kKYes, kKNo,                             // the menu
    kKSeekUp, kKSeekDown, kKPresets, kKStore, kKRedial, kKVoice, kKPhone,   // the source
    kKAudioOn, kKPage, kKView, kKTripReset, kKClose,                         // the drive pages, the preset list
    kKEqPresets,                                                             // the equaliser
  };

 private:
  struct Level {
    uint8_t page = kMHome;
    uint8_t node = 0;       // accessory pages: the node
    uint8_t arg = 0;        // the group / the item / the paired phone
    int sel = 0, top = 0;
  };

  // menu
  void open_menu(uint32_t now_ms);
  void close_menu();
  void push(uint8_t page, uint8_t node = 0, uint8_t arg = 0);
  void pop();
  void build_rows();
  void row_add(uint8_t kind, uint16_t id, uint8_t arg, const char *label, const char *value = "", bool dim = false);
  void menu_move(int steps);
  void menu_click(uint32_t now_ms);
  void edit_turn(int steps, uint32_t now_ms);
  void edit_done(uint32_t now_ms);
  void edit_cancel(uint32_t now_ms);
  void confirm(uint16_t what, uint8_t node, uint8_t arg, int32_t value, const char *question);
  void do_confirmed(uint32_t now_ms);
  void confirm_answer(bool yes, uint32_t now_ms);
  void menu_back();
  const char *source_name(uint8_t code) const;
  void now_text(char *line1, size_t n1, char *line2, size_t n2, uint32_t now_ms) const;

  // display settings (the old OLED menu's items)
  int setting_get(uint16_t id) const;
  void setting_set(uint16_t id, int v, uint32_t now_ms, bool undo = false);
  int setting_count(uint16_t id) const;  // choices (0 = a number range)
  void setting_text(uint16_t id, int v, char *out, size_t n) const;

  // frames and messages out
  void push_frame(const ElcCanFrame &f, const char *what);
  void log(const char *fmt, ...);
  void setting(uint8_t setting, uint8_t value, uint32_t now_ms, const char *what);
  void phone_cmd(uint8_t cmd, uint8_t arg, const char *what);
  bool chan_send(uint16_t base, const ElcChWriter &w, const char *what);
  void media(uint8_t action, uint8_t value, const char *what);
  void play_pause(uint32_t now_ms);
  void request_source(uint8_t code, const char *what, uint32_t now_ms);
  void source_next(uint32_t now_ms);
  void audio_on(uint32_t now_ms);
  void do_key(uint8_t act, uint32_t now_ms);
  void key_hint(uint8_t act, uint32_t now_ms);
  const char *page_name(int page, uint32_t now_ms) const;
  bool reverse_shown() const { return _cs_ms && (_cs.flags & ELC_CAN_STATUS_REVERSE); }
  bool idle_input(uint32_t now_ms);  // the common start of every input: false = ignore it
  void toast(const char *title, const char *value, uint32_t now_ms);
  void cluster_changed(const ElcCanClusterState &old, uint32_t now_ms);
  void build_alerts(uint32_t now_ms);
  uint8_t next_view() const;
  void page_step(int dir, uint32_t now_ms);
  float trip_mi() const;
  void art_pump(uint32_t now_ms);
  void touched(uint32_t now_ms) { _touch_ms = now_ms ? now_ms : 1; }
  void presets_open(uint32_t now_ms);
  /** Close the list and tune to the preset highlighted (when it is stored and not already playing). */
  void presets_choose(const char *why);
  void remote_preset(int n, uint32_t now_ms);
  bool remote_at_rest(uint32_t now_ms);
  bool sel_is_ok(uint32_t now_ms);
  void presets_close(const char *why) {
    if (_pre_open) _pre_why = why;
    _pre_open = false;
  }
  uint8_t sink() const;
  uint8_t radio() const;
  bool bridge_heard(uint32_t now_ms) const { return _bridge_ms && now_ms - _bridge_ms < 3000; }

  // drawing (console_screens.cpp)
  void draw_media(OledGfx &g, uint32_t t);
  void draw_radio(OledGfx &g, uint32_t t);
  void draw_phone_status(OledGfx &g, uint32_t t);
  void draw_trip(OledGfx &g);
  void draw_engine(OledGfx &g, uint32_t t);
  void draw_clock(OledGfx &g, uint32_t t);
  void draw_page(OledGfx &g, int page, uint32_t t);
  void draw_menu(OledGfx &g, uint32_t t);
  void draw_alert(OledGfx &g, const ConsoleAlert &a, uint32_t t);
  void draw_call(OledGfx &g, uint32_t t);
  void draw_pair(OledGfx &g, uint32_t t);
  void draw_presets(OledGfx &g, uint32_t t);
  void draw_aux(OledGfx &g, uint32_t t);
  void draw_eq(OledGfx &g, uint32_t t);
  void draw_softkeys(OledGfx &g, uint32_t t);
  void draw_volume(OledGfx &g, uint32_t t);
  void draw_toast(OledGfx &g, uint32_t t);
  const lv_font_t *font(char role) const;  // h huge, b big, m mid, o body, s small

  uint8_t _node;
  ElcChannels *_ch = nullptr;

  // bus state
  ElcCanSensorDecoder _sdec;
  ConsoleSensors _s;
  uint32_t _sensor_ms = 0, _center_ms = 0, _boot_ms = 0;
  bool _booted = false;
  ElcCanClusterState _cl;
  bool _have_cl = false;
  ElcCanCenterStatus _cs;
  uint32_t _cs_ms = 0;
  ElcCanTime _time;
  uint32_t _time_ms = 0;
  // audio bridge (Bluetooth on the old frames)
  ElcCanBridgeSeen _bridges;
  uint8_t _slot = 0;
  bool _slot_known = false;
  uint32_t _bridge_ms = 0;
  ElcCanAudioTransport _tr;
  uint32_t _tr_ms = 0, _elapsed_moved_ms = 0;
  uint16_t _last_elapsed = 0;
  ElcCanAudioTextDecoder _np;
  ElcCanPhoneStatus _ph;
  uint32_t _ph_ms = 0;
  ElcCanPhoneTextDecoder _caller{ELC_CAN_PHONE_TEXT_CALLER};
  ElcCanPhoneTextDecoder _ph_connected{ELC_CAN_PHONE_TEXT_CONNECTED};
  ElcCanPhoneTextDecoder _ph_pairing{ELC_CAN_PHONE_TEXT_PAIRING};
  ElcCanPhoneTextDecoder _ph_paired[ELC_CAN_PHONE_MAX_PAIRED];
  bool _kinds_set = false;
  uint32_t _passkey = 0;
  uint8_t _pair_left = 0xFF;
  bool _have_passkey = false;
  uint32_t _pair_ms = 0;
  // audio card (the sink) and the active source, on the channel protocol
  ElcAudioStatus _st;
  uint32_t _st_ms = 0;
  ElcAudioNow _now_playing;
  uint32_t _now_ms = 0;
  // album art (48×48 grey)
  uint8_t _art[ELC_CAN_ART_GRAY48_BYTES];
  uint8_t _art_id = 0;  // id shown (0 = none)
  ElcCanArtReceiver _art_rx;
  uint32_t _art_req_ms = 0, _art_rx_ms = 0;
  uint16_t _art_req_have = 0;
  uint8_t _art_tries = 0, _art_failed_id = 0;
  uint32_t _art_starts = 0, _art_reqs = 0, _art_resets = 0;  // diagnostics (`state`)

  // ui
  ConsoleMode _mode = ConsoleMode::Drive;
  uint8_t _page = kPageMedia, _page_from = kPageMedia;
  int8_t _page_dir = 1;
  uint32_t _page_ms = 0;
  Level _stack[6];
  int _depth = 0;
  ConsoleRow _rows[32];
  int _nrows = 0;
  bool _rows_dirty = true;
  bool _advanced = false;
  // the value being edited: a display setting (live, undone on cancel) or an accessory number (sent on confirm)
  uint16_t _edit_id = 0;
  int32_t _edit_val = 0, _edit_orig = 0;
  uint8_t _edit_node = 0, _edit_item = 0;
  // a confirmation page waiting
  uint16_t _cf_what = 0;
  uint8_t _cf_node = 0, _cf_arg = 0;
  int32_t _cf_value = 0;
  char _cf_question[48] = {0};
  bool _ack[8] = {false};
  ConsoleAlert _alerts[8];
  int _n_alerts = 0;
  char _toast_title[16] = {0}, _toast_value[24] = {0};
  uint32_t _toast_ms = 0;
  uint32_t _own_change_ms = 0;  // a setting the console itself asked for: no toast for its echo
  uint8_t _vol = 0;             // 0..15 (the bar)
  uint32_t _vol_ms = 0;        // the last volume change
  uint32_t _vol_shown_ms = 0;  // when the bar appeared (it slides in once, not per detent)
  bool _vol_set = false;
  uint32_t _vol_set_ms = 0;
  uint8_t _view_req = 0;
  uint32_t _view_req_ms = 0;
  int _bright_live = -1;  // brightness while being edited (the center's echo lags)
  uint32_t _touch_ms = 0;
  bool _pre_open = false;  // the preset list
  int _pre_sel = 0;
  uint32_t _pre_ms = 0;    // its last input
  const char *_pre_why = "";  // why it closed last (the serial console's state)
  uint8_t _src_req = 0;   // the source asked for, until STATUS says so
  uint8_t _on_src = ELC_SRC_BLUETOOTH;  // the source to put back when the audio is switched on again
  bool _bt_was_playing = true;
  bool _was_call = false;     // a call showed at the last tick
  uint8_t _call_src = 0xFE;   // the source when the call began (0xFE = not known): put back when it ends
  uint32_t _src_req_ms = 0;

  // persistent
  bool _trip_set = false;
  uint32_t _trip_base = 0;
  ConsoleContrast _contrast = ConsoleContrast::Auto;
  bool _persist_dirty = false;

  ConsoleAction _q[16];
  uint8_t _qh = 0, _qn = 0;
  char _log[4][72];
  uint8_t _lh = 0, _ln = 0;
  uint32_t _now = 0;
};
