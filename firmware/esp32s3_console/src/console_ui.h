#pragma once

// The console's behaviour, as designed in the El Camino Cluster prototype (docs/protocols/can_bus.md, "Console"):
// four drive pages, an OLED menu of quick settings, the round-screen menu driven by the knob, warnings and calls
// that take over, toasts and a volume bar. No Arduino dependency: it takes CAN frames and knob/key input with a clock
// and queues the frames to send, so it runs in the host tests and the host preview. Drawing: console_screens.cpp.

#include <stddef.h>
#include <stdint.h>

#include <elc_can_codec.h>

class OledGfx;
struct _lv_font_t;
typedef struct _lv_font_t lv_font_t;

/** A frame the console wants on the bus. */
struct ConsoleAction {
  ElcCanFrame frame;
  char what[48];  // for the serial log
};

enum class ConsoleMode : uint8_t { Drive, Menu, Edit, Remote };
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
  float tank = 20;
};

struct ConsoleAlert {
  const char *id;
  const char *title;
  char line[64];
};

class ConsoleUi {
 public:
  explicit ConsoleUi(uint8_t node = ELC_CAN_NODE_CONSOLE) : _node(node) {}

  /** Persistent settings kept by the console (NVS in the firmware). */
  void restore(bool trip_set, uint32_t trip_base_tenths, ConsoleContrast contrast);
  bool trip_set() const { return _trip_set; }
  uint32_t trip_base() const { return _trip_base; }
  ConsoleContrast contrast() const { return _contrast; }
  /** True once after a persistent setting changed (the firmware saves them). */
  bool take_persist_dirty();

  // ---- input
  void feed(const ElcCanFrame &f, uint32_t now_ms);
  void select_turn(int steps, uint32_t now_ms);
  void select_press(uint32_t now_ms);
  void back(uint32_t now_ms);  // left key
  void view(uint32_t now_ms);  // right key
  void volume_turn(int steps, uint32_t now_ms);
  void volume_press(uint32_t now_ms);
  void volume_hold(uint32_t now_ms);
  void tick(uint32_t now_ms);

  bool pop_action(ConsoleAction &out);

  // ---- output
  /** Draws the whole screen for time now_ms. */
  void render(OledGfx &g, uint32_t now_ms);
  /** 0..1 panel brightness for the grey levels (OLED contrast setting and the cluster brightness). */
  float level_scale() const;
  /** Key LEDs: 0..1 (they follow the cluster brightness and pulse while a call rings). */
  float led_level(uint32_t now_ms) const;
  /** One line per state, for the serial console. */
  void describe(char *out, size_t n, uint32_t now_ms) const;

  // ---- state (tests, serial console)
  ConsoleMode mode() const { return _mode; }
  uint8_t page() const { return _page; }
  uint8_t menu_sel() const { return _sel; }
  bool alive(uint32_t now_ms) const;
  const ConsoleSensors &sensors() const { return _s; }
  const ElcCanClusterState &cluster() const { return _cl; }
  const ElcCanPhoneStatus &phone() const { return _ph; }
  bool phone_fresh(uint32_t now_ms) const { return _ph_ms && now_ms - _ph_ms < 3000; }
  bool call_shown(uint32_t now_ms) const;
  const ConsoleAlert *alert(uint32_t now_ms);  // the unacknowledged warning shown, or null
  uint8_t bridge_slot() const { return _slot; }
  uint8_t volume() const { return _vol; }

  static const int kItems = 10;
  static constexpr uint32_t kCenterGoneMs = 2500;  // the center's heartbeat is 1 Hz: a couple missed before the bowtie
  static constexpr uint32_t kBootMs = 1700;        // bowtie at power-up: fade in, then a sheen
  static constexpr uint32_t kToastMs = 1800;
  static constexpr uint32_t kVolumeShowMs = 1450;
  const char *item_label(int i) const;
  void item_value(int i, char *out, size_t n) const;

 private:
  // settings in the OLED menu
  enum Item : uint8_t { kView, kTheme, kBright, kFollow, kIface, kMedia, kLayout, kTrip, kContrast, kService };
  int item_get(int i) const;
  void item_set(int i, int v, uint32_t now_ms, bool undo = false);
  int item_count(int i) const;  // choices (0 = range / action / page)

  void push_frame(const ElcCanFrame &f, const char *what);
  void setting(uint8_t setting, uint8_t value, uint32_t now_ms, const char *what);
  void phone_cmd(uint8_t cmd, uint8_t arg, const char *what);
  void menu_input(uint8_t event, int8_t value, const char *what);
  void toast(const char *title, const char *value, uint32_t now_ms);
  void cluster_changed(const ElcCanClusterState &old, uint32_t now_ms);
  void build_alerts(uint32_t now_ms);
  bool remote_open(uint32_t now_ms) const;
  uint8_t next_view() const;
  void page_step(int dir, uint32_t now_ms);
  float trip_mi() const;
  void art_pump(uint32_t now_ms);

  // drawing (console_screens.cpp)
  void draw_media(OledGfx &g, uint32_t t);
  void draw_trip(OledGfx &g);
  void draw_engine(OledGfx &g, uint32_t t);
  void draw_clock(OledGfx &g, uint32_t t);
  void draw_page(OledGfx &g, int page, uint32_t t);
  void draw_menu(OledGfx &g, uint32_t t);
  void draw_remote(OledGfx &g);
  void draw_alert(OledGfx &g, const ConsoleAlert &a, uint32_t t);
  void draw_call(OledGfx &g, uint32_t t);
  void draw_volume(OledGfx &g, uint32_t t);
  void draw_toast(OledGfx &g, uint32_t t);
  const lv_font_t *font(char role) const;  // h huge, b big, m mid, o body, s small

  uint8_t _node;

  // bus state
  ElcCanSensorDecoder _sdec;
  ConsoleSensors _s;
  uint32_t _sensor_ms = 0, _center_ms = 0, _boot_ms = 0;
  bool _booted = false;
  ElcCanClusterState _cl;
  bool _have_cl = false;
  ElcCanCenterStatus _cs;
  uint32_t _cs_ms = 0;
  ElcCanMenuState _ms;
  uint32_t _ms_ms = 0;
  ElcCanTextDecoder _mt_main{ELC_CAN_ID_MENU_TEXT_MAIN}, _mt_sub{ELC_CAN_ID_MENU_TEXT_SUB};
  ElcCanTime _time;
  uint32_t _time_ms = 0;
  // audio bridge
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
  bool _remote_by_console = false;
  ConsoleMode _mode_before_remote = ConsoleMode::Drive;
  uint8_t _page = kPageMedia, _page_from = kPageMedia;
  int8_t _page_dir = 1;
  uint32_t _page_ms = 0;
  uint8_t _sel = 0, _top = 0;
  int _edit_orig = 0;
  bool _confirm = false;
  bool _ack[8] = {false};
  ConsoleAlert _alerts[8];
  int _n_alerts = 0;
  char _toast_title[16] = {0}, _toast_value[24] = {0};
  uint32_t _toast_ms = 0;
  uint32_t _own_change_ms = 0;  // a setting the console itself asked for: no toast for its echo
  uint8_t _vol = 0;
  uint32_t _vol_ms = 0;
  bool _vol_set = false;
  uint32_t _vol_set_ms = 0;
  uint8_t _view_req = 0;
  uint32_t _view_req_ms = 0;
  int _bright_live = -1;  // brightness while being edited (the center's echo lags)

  // persistent
  bool _trip_set = false;
  uint32_t _trip_base = 0;
  ConsoleContrast _contrast = ConsoleContrast::Auto;
  bool _persist_dirty = false;

  ConsoleAction _q[16];
  uint8_t _qh = 0, _qn = 0;
  uint32_t _now = 0;
};
