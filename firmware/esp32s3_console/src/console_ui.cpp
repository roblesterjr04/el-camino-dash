#include "console_ui.h"

#include <elc_sensor_units.h>
#include <stdio.h>
#include <string.h>

#ifndef WHEEL_CIRCUMFERENCE_M
#define WHEEL_CIRCUMFERENCE_M 2.1f  // same default as the displays
#endif

namespace {

constexpr uint32_t kSensorFreshMs = 1000;
constexpr uint32_t kOwnEchoMs = 2000;     // our own setting's echo does not toast
constexpr uint32_t kVolHoldMs = 1500;     // our volume wins over the bridge's stale echo
constexpr uint32_t kArtQuietMs = 300;
constexpr uint8_t kArtMaxTries = 20;

const char *const kViewName[4] = {"Tachometer", "Clock", "Media", "Efficiency"};
const char *const kScreenName[3] = {"Left", "Center", "Right"};
// OLED menu order Classic, Modern, Fallout; on the wire 0 Classic, 1 Fallout, 2 Modern.
const uint8_t kThemeWire[3] = {0, 2, 1};
const char *const kThemeName[3] = {"Classic", "Modern", "Fallout"};

int theme_index(uint8_t wire) { return wire == 2 ? 1 : (wire == 1 ? 2 : 0); }
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

void ConsoleUi::restore(bool trip_set, uint32_t trip_base_tenths, ConsoleContrast contrast) {
  _trip_set = trip_set;
  _trip_base = trip_base_tenths;
  _contrast = contrast;
}

bool ConsoleUi::take_persist_dirty() {
  const bool d = _persist_dirty;
  _persist_dirty = false;
  return d;
}

// ---- frames out ----

void ConsoleUi::push_frame(const ElcCanFrame &f, const char *what) {
  if (_qn == sizeof(_q) / sizeof(_q[0])) {  // full: drop the oldest
    _qh = (uint8_t)((_qh + 1) % (sizeof(_q) / sizeof(_q[0])));
    _qn--;
  }
  ConsoleAction &a = _q[(_qh + _qn) % (sizeof(_q) / sizeof(_q[0]))];
  a.frame = f;
  snprintf(a.what, sizeof(a.what), "%s", what);
  _qn++;
}

bool ConsoleUi::pop_action(ConsoleAction &out) {
  if (!_qn) return false;
  out = _q[_qh];
  _qh = (uint8_t)((_qh + 1) % (sizeof(_q) / sizeof(_q[0])));
  _qn--;
  return true;
}

void ConsoleUi::setting(uint8_t s, uint8_t v, uint32_t now_ms, const char *what) {
  ElcCanSettingRequest r;
  r.setting = s;
  r.value = v;
  r.from_node = _node;
  ElcCanFrame f;
  elc_can_encode_setting_request(r, f);
  _own_change_ms = now_ms;
  char buf[48];
  snprintf(buf, sizeof(buf), "SET %s = %u", what, v);
  push_frame(f, buf);
}

void ConsoleUi::phone_cmd(uint8_t cmd, uint8_t arg, const char *what) {
  if (!_slot_known) return;  // no bridge heard yet: nobody to send it to
  ElcCanPhoneCommandMsg c;
  c.slot = _slot;
  c.command = cmd;
  c.arg = arg;
  c.from_node = _node;
  c.list_serial = _ph.list_serial;
  ElcCanFrame f;
  elc_can_encode_phone_command(c, f);
  char buf[48];
  snprintf(buf, sizeof(buf), "PHONE %s %u", what, arg);
  push_frame(f, buf);
}

void ConsoleUi::menu_input(uint8_t event, int8_t value, const char *what) {
  ElcCanMenuInput m;
  m.event = event;
  m.value = value;
  m.from_node = _node;
  ElcCanFrame f;
  elc_can_encode_menu_input(m, f);
  char buf[48];
  snprintf(buf, sizeof(buf), "MENU %s %d", what, value);
  push_frame(f, buf);
}

void ConsoleUi::toast(const char *title, const char *value, uint32_t now_ms) {
  snprintf(_toast_title, sizeof(_toast_title), "%s", title);
  snprintf(_toast_value, sizeof(_toast_value), "%s", value);
  _toast_ms = now_ms ? now_ms : 1;
}

// ---- frames in ----

void ConsoleUi::cluster_changed(const ElcCanClusterState &old, uint32_t now_ms) {
  if (!_have_cl || now_ms - _own_change_ms < kOwnEchoMs) return;  // first state, or the echo of our own change
  if (old.theme != _cl.theme) toast("THEME", kThemeName[theme_index(_cl.theme)], now_ms);
  if (old.center_subview != _cl.center_subview) toast("VIEW", kViewName[_cl.center_subview & 3], now_ms);
  if (old.interface_screen != _cl.interface_screen) toast("MENU ON", kScreenName[_cl.interface_screen % 3], now_ms);
  if (old.media_screen != _cl.media_screen) toast("MEDIA ON", kScreenName[_cl.media_screen % 3], now_ms);
}

void ConsoleUi::feed(const ElcCanFrame &f, uint32_t now_ms) {
  _now = now_ms;
  // Audio frames only from a range whose heartbeat says audio bridge: node numbers follow the card slot, so another
  // card's private messages can sit at the same offsets.
  _bridges.feed(f, now_ms);
  if (_bridges.reject(f, now_ms)) return;
  if (f.id == ELC_CAN_ID_HEARTBEAT_BASE + ELC_CAN_NODE_CENTER) {
    _center_ms = now_ms;
    _booted = true;
    return;
  }
  if (_sdec.feed(f)) {
    _center_ms = _sensor_ms = now_ms;
    _booted = true;
    const ElcCanSensor &v = _sdec.value();
    _s.mph = (float)v.speed_rpm / 60.0f * WHEEL_CIRCUMFERENCE_M * 2.23694f;
    _s.rpm = (float)v.engine_rpm;
    _s.fuel = elc_units_fuel_pct(v.pot_adc);
    _s.oil = elc_units_oil_psi(v.pressure_adc);
    _s.coolant = elc_units_coolant_f(v.therm_adc);
    _s.amp = elc_units_amp(v.voltage_adc);
    _s.charging = !(v.flags & ELC_SENSOR_FLAG_IGN_ON) ? -1 : ((v.flags & ELC_SENSOR_FLAG_ALT_OK) ? 1 : 0);
    _s.odo_valid = v.odometer_tenths_mile != 0xFFFFFFFFu;
    _s.odo_tenths = _s.odo_valid ? v.odometer_tenths_mile : 0;
    _s.econ = v.has_fuel_economy;
    _s.avg = v.mpg_average_x10 * 0.1f;
    _s.inst = v.mpg_instant_x10 * 0.1f;
    _s.dte = v.dte_tenths_mile * 0.1f;
    return;
  }
  ElcCanClusterState cl;
  if (elc_can_decode_cluster_state(f, cl)) {
    const ElcCanClusterState old = _cl;
    _cl = cl;
    cluster_changed(old, now_ms);
    _have_cl = true;
    _center_ms = now_ms;
    _booted = true;
    if (_bright_live >= 0 && now_ms - _own_change_ms >= kOwnEchoMs) _bright_live = -1;
    return;
  }
  if (elc_can_decode_center_status(f, _cs)) {
    _cs_ms = _center_ms = now_ms;
    _booted = true;
    return;
  }
  if (_mt_main.feed(f) || _mt_sub.feed(f)) return;
  if (elc_can_decode_menu_state(f, _ms)) {
    _ms_ms = now_ms;
    return;
  }
  if (elc_can_decode_time(f, _time)) {
    _time_ms = now_ms;
    return;
  }

  // audio bridge: follow the first one heard
  uint8_t slot = 0;
  ElcCanAudioTransport tr;
  if (elc_can_decode_audio_transport(f, &slot, tr)) {
    if (_slot_known && slot != _slot && now_ms - _bridge_ms < 3000) return;
    _slot = slot;
    _slot_known = true;
    _bridge_ms = _tr_ms = now_ms;
    if (tr.elapsed_s != _last_elapsed) _elapsed_moved_ms = now_ms;
    _last_elapsed = tr.elapsed_s;
    _tr = tr;
    if (!_vol_set || now_ms - _vol_set_ms > kVolHoldMs) _vol = tr.volume > 15 ? 15 : tr.volume;
    return;
  }
  ElcCanPhoneStatus ps;
  if (elc_can_decode_phone_status(f, &slot, ps)) {
    if (_slot_known && slot != _slot && now_ms - _bridge_ms < 3000) return;
    _slot = slot;
    _slot_known = true;
    _bridge_ms = _ph_ms = now_ms;
    _ph = ps;
    return;
  }
  const int s = elc_can_slot_of(f.id);
  if (s >= 0 && _slot_known && s != _slot) return;
  if (_np.feed(f) || _caller.feed(f)) return;
  if (_art_rx.feed(f)) _art_rx_ms = now_ms;
}

// ---- state ----

bool ConsoleUi::alive(uint32_t now_ms) const {
  return _booted && now_ms - _center_ms < kCenterGoneMs && now_ms - _boot_ms >= kBootMs;
}

bool ConsoleUi::remote_open(uint32_t now_ms) const { return _ms_ms && _ms.open && now_ms - _ms_ms < 2500; }

bool ConsoleUi::call_shown(uint32_t now_ms) const { return phone_fresh(now_ms) && _ph.call != ELC_CAN_CALL_IDLE; }

float ConsoleUi::trip_mi() const {
  return _s.odo_valid && _trip_set && _s.odo_tenths >= _trip_base ? (float)(_s.odo_tenths - _trip_base) / 10.0f : 0.0f;
}

uint8_t ConsoleUi::next_view() const {
  const bool held = _view_req_ms && _now - _view_req_ms <= kOwnEchoMs;
  uint8_t n = (uint8_t)(((held ? _view_req : _cl.center_subview) + 1) & 3);
  if (n == 2 && _cl.media_screen != ELC_CAN_SCREEN_CENTER) n = 3;  // media is a view only on the media screen
  return n;
}

void ConsoleUi::build_alerts(uint32_t now_ms) {
  _n_alerts = 0;
  bool live[8] = {false};
  auto add = [&](int k, const char *id, const char *title) -> ConsoleAlert & {
    live[k] = true;
    ConsoleAlert &a = _alerts[_n_alerts++];
    a.id = id;
    a.title = title;
    a.line[0] = '\0';
    return a;
  };
  if (alive(now_ms) && !(_cs_ms && (_cs.flags & ELC_CAN_STATUS_REVERSE))) {
    const bool fresh = _sensor_ms && now_ms - _sensor_ms < kSensorFreshMs;
    if (!fresh) {
      snprintf(add(0, "nodata", "NO SENSOR DATA").line, 64, "Gauges show NO DATA \xC2\xB7 check sensor pack");
    } else {
      if (_s.rpm > 400 && _s.oil < 8) {
        snprintf(add(1, "oil", "LOW OIL PRESSURE").line, 64, "%d psi at %d rpm \xC2\xB7 stop when safe", (int)(_s.oil + 0.5f),
                 (int)(_s.rpm + 0.5f));
      }
      if (_s.coolant > 230) {
        snprintf(add(2, "hot", "ENGINE HOT").line, 64, "Coolant %d\xC2\xB0" "F \xC2\xB7 ease off, watch the gauge",
                 (int)(_s.coolant + 0.5f));
      }
      if (_s.charging == 0 && _s.rpm > 400) {
        snprintf(add(3, "gen", "NOT CHARGING").line, 64, "Running on battery \xC2\xB7 check alternator");
      }
      if (_s.fuel < 12) {
        snprintf(add(4, "fuel", "LOW FUEL").line, 64, "%.1f gal \xC2\xB7 about %d mi left", _s.fuel / 100 * _s.tank,
                 (int)(_s.dte + 0.5f));
      }
      if (!_s.odo_valid) {
        snprintf(add(5, "odo", "ODOMETER NOT STORED").line, 64, "FRAM not answering \xC2\xB7 miles kept in RAM");
      }
    }
  }
  for (int k = 0; k < 8; k++) {
    if (!live[k]) _ack[k] = false;  // re-arms once the condition clears
  }
}

const ConsoleAlert *ConsoleUi::alert(uint32_t now_ms) {
  build_alerts(now_ms);
  static const char *const kIds[6] = {"nodata", "oil", "hot", "gen", "fuel", "odo"};
  for (int i = 0; i < _n_alerts; i++) {
    for (int k = 0; k < 6; k++) {
      if (!strcmp(_alerts[i].id, kIds[k]) && !_ack[k]) return &_alerts[i];
    }
  }
  return nullptr;
}

// ---- OLED menu items ----

const char *ConsoleUi::item_label(int i) const {
  static const char *const kLabel[kItems] = {"Center view", "Theme",   "Brightness",    "Dim with lights",
                                             "Menu on",     "Media on", "Right layout", "Trip A",
                                             "OLED contrast", "Service"};
  return i >= 0 && i < kItems ? kLabel[i] : "";
}

int ConsoleUi::item_count(int i) const {
  switch (i) {
    case kView: return 4;
    case kTheme: return 3;
    case kFollow: return 2;
    case kIface: case kMedia: return 3;
    case kLayout: return 2;
    case kContrast: return 3;
    default: return 0;
  }
}

int ConsoleUi::item_get(int i) const {
  switch (i) {
    case kView: return _cl.center_subview & 3;
    case kTheme: return theme_index(_cl.theme);
    case kBright: return _bright_live >= 0 ? _bright_live : _cl.brightness;
    case kFollow: return (_cl.flags & ELC_CAN_CLUSTER_DIM_WITH_LIGHTS) ? 0 : 1;  // On, Off
    case kIface: return _cl.interface_screen % 3;
    case kMedia: return _cl.media_screen % 3;
    case kLayout: return (_cl.flags & ELC_CAN_CLUSTER_RIGHT_LAYOUT2) ? 1 : 0;
    case kContrast: return (int)_contrast;
    default: return 0;
  }
}

void ConsoleUi::item_set(int i, int v, uint32_t now_ms, bool undo) {
  const char *u = undo ? " (undo)" : "";
  char what[32];
  switch (i) {
    case kView:
      snprintf(what, sizeof(what), "view%s", u);
      _cl.center_subview = (uint8_t)v;  // show the choice at once; the center's 0x210 confirms it
      setting(ELC_CAN_SET_CENTER_VIEW, (uint8_t)v, now_ms, what);
      break;
    case kTheme:
      snprintf(what, sizeof(what), "theme%s", u);
      _cl.theme = kThemeWire[v];
      setting(ELC_CAN_SET_THEME, kThemeWire[v], now_ms, what);
      break;
    case kBright:
      snprintf(what, sizeof(what), "brightness%s", u);
      _bright_live = v;
      setting(ELC_CAN_SET_BRIGHTNESS, (uint8_t)v, now_ms, what);
      break;
    case kFollow:
      snprintf(what, sizeof(what), "dim-with-lights%s", u);
      _cl.flags = (uint8_t)(v == 0 ? (_cl.flags | ELC_CAN_CLUSTER_DIM_WITH_LIGHTS) : (_cl.flags & ~ELC_CAN_CLUSTER_DIM_WITH_LIGHTS));
      setting(ELC_CAN_SET_DIM_WITH_LIGHTS, v == 0 ? 1 : 0, now_ms, what);
      break;
    case kIface:
      snprintf(what, sizeof(what), "menu-on%s", u);
      _cl.interface_screen = (uint8_t)v;
      setting(ELC_CAN_SET_INTERFACE_SCREEN, (uint8_t)v, now_ms, what);
      break;
    case kMedia:
      snprintf(what, sizeof(what), "media-on%s", u);
      _cl.media_screen = (uint8_t)v;
      setting(ELC_CAN_SET_MEDIA_SCREEN, (uint8_t)v, now_ms, what);
      break;
    case kLayout:
      snprintf(what, sizeof(what), "right-layout%s", u);
      _cl.flags = (uint8_t)(v ? (_cl.flags | ELC_CAN_CLUSTER_RIGHT_LAYOUT2) : (_cl.flags & ~ELC_CAN_CLUSTER_RIGHT_LAYOUT2));
      setting(ELC_CAN_SET_RIGHT_LAYOUT, (uint8_t)v, now_ms, what);
      break;
    case kContrast:
      _contrast = (ConsoleContrast)v;
      _persist_dirty = true;
      break;
    default: break;
  }
}

void ConsoleUi::item_value(int i, char *out, size_t n) const {
  const int v = item_get(i);
  switch (i) {
    case kView: snprintf(out, n, "%s", kViewName[v & 3]); break;
    case kTheme: snprintf(out, n, "%s", kThemeName[v % 3]); break;
    case kBright: snprintf(out, n, "%d%%", v); break;
    case kFollow: snprintf(out, n, "%s", v == 0 ? "On" : "Off"); break;
    case kIface: case kMedia: snprintf(out, n, "%s", kScreenName[v % 3]); break;
    case kLayout: snprintf(out, n, "%s", v ? "Fuel + lamps" : "Four gauges"); break;
    case kTrip: snprintf(out, n, "%.1f mi", trip_mi()); break;
    case kContrast: snprintf(out, n, "%s", v == 0 ? "Auto" : (v == 1 ? "Bright" : "Night")); break;
    case kService: snprintf(out, n, "Diagnostics"); break;
    default: out[0] = '\0'; break;
  }
}

// ---- input ----

void ConsoleUi::page_step(int dir, uint32_t now_ms) {
  _page_from = _page;
  _page = (uint8_t)((_page + dir + kPageCount) % kPageCount);
  _page_ms = now_ms;
  _page_dir = (int8_t)dir;
}

void ConsoleUi::select_turn(int steps, uint32_t now_ms) {
  _now = now_ms;
  if (!steps || !alive(now_ms)) return;
  if (call_shown(now_ms)) return;
  if (_mode == ConsoleMode::Remote) {
    menu_input(ELC_CAN_MENU_ROTATE, (int8_t)clampi(steps, -127, 127), "rotate");
    return;
  }
  if (_mode == ConsoleMode::Edit) {
    const int i = _sel;
    if (i == kBright) {
      const int v = clampi(item_get(i) + steps * 5, 10, 100);
      if (v != item_get(i)) item_set(i, v, now_ms);
    } else if (item_count(i)) {
      const int v = clampi(item_get(i) + steps, 0, item_count(i) - 1);
      if (v != item_get(i)) item_set(i, v, now_ms);
    }
    return;
  }
  if (_mode == ConsoleMode::Menu) {
    _sel = (uint8_t)clampi(_sel + steps, 0, kItems - 1);
    return;
  }
  if (alert(now_ms)) return;
  page_step(steps > 0 ? 1 : -1, now_ms);
}

void ConsoleUi::select_press(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms) || call_shown(now_ms)) return;
  if (_mode != ConsoleMode::Remote && _mode != ConsoleMode::Edit) {
    build_alerts(now_ms);
    static const char *const kIds[6] = {"nodata", "oil", "hot", "gen", "fuel", "odo"};
    const ConsoleAlert *a = alert(now_ms);
    if (a) {
      for (int k = 0; k < 6; k++) {
        if (!strcmp(a->id, kIds[k])) _ack[k] = true;
      }
      return;
    }
  }
  switch (_mode) {
    case ConsoleMode::Remote: menu_input(ELC_CAN_MENU_PRESS, 0, "press"); break;
    case ConsoleMode::Drive: _mode = ConsoleMode::Menu; break;
    case ConsoleMode::Menu:
      if (_sel == kService) {
        _mode_before_remote = ConsoleMode::Menu;
        _remote_by_console = true;
        menu_input(ELC_CAN_MENU_OPEN, ELC_CAN_PAGE_SERVICE, "open");
        _mode = ConsoleMode::Remote;
        _ms.open = true;  // until the interface screen answers
        _ms.opened_by = ELC_CAN_MENU_BY_CONSOLE;
        _ms.page = ELC_CAN_PAGE_SERVICE;
        _ms.screen = _cl.interface_screen;
        _ms_ms = now_ms;
      } else {
        _mode = ConsoleMode::Edit;
        _confirm = _sel == kTrip;
        _edit_orig = item_get(_sel);
      }
      break;
    case ConsoleMode::Edit:
      if (_sel == kTrip && _s.odo_valid) {
        _trip_base = _s.odo_tenths;
        _trip_set = true;
        _persist_dirty = true;
      }
      _mode = ConsoleMode::Menu;
      _confirm = false;
      break;
  }
}

void ConsoleUi::back(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms)) return;
  if (call_shown(now_ms)) {  // soft key: Decline / Ignore / End
    if (_ph.call == ELC_CAN_CALL_WAITING) phone_cmd(ELC_CAN_PHONE_CMD_CALL_WAITING, 0, "ignore");
    else phone_cmd(ELC_CAN_PHONE_CMD_END, 0, _ph.call == ELC_CAN_CALL_INCOMING ? "decline" : "end");
    return;
  }
  if (_mode != ConsoleMode::Remote && _mode != ConsoleMode::Edit) {
    static const char *const kIds[6] = {"nodata", "oil", "hot", "gen", "fuel", "odo"};
    const ConsoleAlert *a = alert(now_ms);
    if (a) {
      for (int k = 0; k < 6; k++) {
        if (!strcmp(a->id, kIds[k])) _ack[k] = true;
      }
      return;
    }
  }
  switch (_mode) {
    case ConsoleMode::Remote: menu_input(ELC_CAN_MENU_BACK, 0, "back"); break;
    case ConsoleMode::Edit:
      if (item_count(_sel) || _sel == kBright) {
        if (item_get(_sel) != _edit_orig) item_set(_sel, _edit_orig, now_ms, true);
      }
      _mode = ConsoleMode::Menu;
      _confirm = false;
      break;
    case ConsoleMode::Menu: _mode = ConsoleMode::Drive; break;
    case ConsoleMode::Drive:
      if (_page != kPageMedia) {
        _page_from = _page;
        _page = kPageMedia;
        _page_ms = now_ms;
        _page_dir = -1;
      }
      break;
  }
}

void ConsoleUi::view(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms)) return;
  if (call_shown(now_ms)) {  // soft key: Answer / Mute / Swap
    switch (_ph.call) {
      case ELC_CAN_CALL_INCOMING: phone_cmd(ELC_CAN_PHONE_CMD_ANSWER, 0, "answer"); break;
      case ELC_CAN_CALL_WAITING: phone_cmd(ELC_CAN_PHONE_CMD_CALL_WAITING, 2, "answer (hold)"); break;
      case ELC_CAN_CALL_ACTIVE:
        phone_cmd(ELC_CAN_PHONE_CMD_MIC_MUTE, (_ph.flags & ELC_CAN_PHONE_MIC_MUTED) ? 0 : 1, "mute");
        break;
      case ELC_CAN_CALL_HELD: phone_cmd(ELC_CAN_PHONE_CMD_CALL_WAITING, 2, "swap"); break;
      default: break;  // dialing / alerting: only End
    }
    return;
  }
  if (_cs_ms && (_cs.flags & ELC_CAN_STATUS_REVERSE)) return;
  const uint8_t v = next_view();
  _view_req = v;
  _view_req_ms = now_ms;
  setting(ELC_CAN_SET_CENTER_VIEW, v, now_ms, "view");
}

void ConsoleUi::volume_turn(int steps, uint32_t now_ms) {
  _now = now_ms;
  if (!steps || !alive(now_ms)) return;
  _vol_ms = now_ms;
  if (call_shown(now_ms)) {
    const int v = clampi(_ph.call_volume + steps, 0, 15);
    _ph.call_volume = (uint8_t)v;
    phone_cmd(ELC_CAN_PHONE_CMD_CALL_VOLUME, (uint8_t)v, "call-volume");
    return;
  }
  const int v = clampi(_vol + steps, 0, 15);
  if (v == _vol) return;
  _vol = (uint8_t)v;
  _vol_set = true;
  _vol_set_ms = now_ms;
  phone_cmd(ELC_CAN_PHONE_CMD_MEDIA_VOLUME, (uint8_t)v, "volume");
}

void ConsoleUi::volume_press(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms)) return;
  if (call_shown(now_ms)) {
    if (_ph.call == ELC_CAN_CALL_ACTIVE || _ph.call == ELC_CAN_CALL_HELD || _ph.call == ELC_CAN_CALL_WAITING) {
      phone_cmd(ELC_CAN_PHONE_CMD_MIC_MUTE, (_ph.flags & ELC_CAN_PHONE_MIC_MUTED) ? 0 : 1, "mute");
    }
    return;
  }
  phone_cmd(ELC_CAN_PHONE_CMD_MEDIA, 0, "play/pause");
}

void ConsoleUi::volume_hold(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms) || call_shown(now_ms)) return;
  phone_cmd(ELC_CAN_PHONE_CMD_MEDIA, 1, "next");
}

void ConsoleUi::art_pump(uint32_t now_ms) {
  // A few lost transport frames do not drop the cover (that restarted the download and flashed the art); a phone
  // that disconnects, or a bridge silent for 10 s, does.
  const bool bridge = _tr_ms && now_ms - _tr_ms < 10000 && _tr.connected;
  const uint8_t want = bridge ? _tr.art_id : 0;
  if (want == 0) {
    if (_art_id || _art_rx.art_id()) _art_resets++;
    _art_id = 0;
    _art_rx.reset();
    return;
  }
  if (want == _art_id || want == _art_failed_id) return;
  static uint8_t s_dl[ELC_CAN_ART_GRAY48_BYTES];
  if (_art_rx.art_id() != want || _art_rx.variant() != ELC_CAN_ART_GRAY48) {
    _art_rx.start(_slot, want, ELC_CAN_ART_GRAY48, s_dl, sizeof(s_dl));
    _art_starts++;
    _art_req_ms = 0;
    _art_tries = 0;
    _art_req_have = 0;
  }
  if (_art_rx.refused()) {
    _art_failed_id = want;
    return;
  }
  if (_art_rx.complete()) {
    memcpy(_art, s_dl, sizeof(_art));
    _art_id = want;
    return;
  }
  const uint32_t last = _art_rx_ms > _art_req_ms ? _art_rx_ms : _art_req_ms;
  if (_art_req_ms == 0 || now_ms - last >= kArtQuietMs) {
    if (_art_req_ms && _art_rx.chunks_have() == _art_req_have && ++_art_tries >= kArtMaxTries) {
      _art_failed_id = want;
      return;
    }
    ElcCanArtRequest r;
    r.slot = _slot;
    r.art_id = want;
    r.first_chunk = _art_rx.first_missing();
    r.variant = ELC_CAN_ART_GRAY48;
    ElcCanFrame f;
    elc_can_encode_art_request(r, f);
    push_frame(f, "ART request (grey 48)");
    _art_reqs++;
    _art_req_ms = now_ms;
    _art_req_have = _art_rx.chunks_have();
  }
}

void ConsoleUi::tick(uint32_t now_ms) {
  _now = now_ms;
  if (_boot_ms == 0) _boot_ms = now_ms ? now_ms : 1;
  // The round-screen menu open (by touch or by us): the knob drives it; closed: back to where we were.
  if (remote_open(now_ms) && _mode != ConsoleMode::Remote) {
    _mode_before_remote = _mode == ConsoleMode::Edit ? ConsoleMode::Menu : _mode;
    _remote_by_console = _ms.opened_by == ELC_CAN_MENU_BY_CONSOLE;
    _mode = ConsoleMode::Remote;
  } else if (!remote_open(now_ms) && _mode == ConsoleMode::Remote) {
    _mode = _remote_by_console ? ConsoleMode::Menu : _mode_before_remote;
    _remote_by_console = false;
  }
  if (_mode == ConsoleMode::Edit && !alive(now_ms)) _mode = ConsoleMode::Menu;
  art_pump(now_ms);
}

float ConsoleUi::level_scale() const {
  if (_contrast == ConsoleContrast::Bright) return 1.0f;
  if (_contrast == ConsoleContrast::Night) return 0.45f;
  const float b = (_bright_live >= 0 ? _bright_live : _cl.brightness) / 100.0f;
  return b < 0.35f ? 0.35f : b;
}

float ConsoleUi::led_level(uint32_t now_ms) const {
  const float base = level_scale() * 0.6f;
  if (call_shown(now_ms) && (_ph.call == ELC_CAN_CALL_INCOMING || _ph.call == ELC_CAN_CALL_WAITING)) {
    const uint32_t p = now_ms % 1000;
    return (p < 500 ? p : 1000 - p) / 500.0f;  // pulse while it rings
  }
  return base;
}

void ConsoleUi::describe(char *out, size_t n, uint32_t now_ms) const {
  static const char *const kMode[4] = {"drive", "menu", "edit", "remote"};
  static const char *const kPage[4] = {"media", "trip", "engine", "clock"};
  static const char *const kCall[7] = {"idle", "incoming", "dialing", "alerting", "active", "waiting", "held"};
  char v[24];
  item_value(_sel, v, sizeof(v));
  snprintf(out, n,
           "mode %s, page %s, menu item %d (%s = %s); center %s, sensors %s, bridge %s (slot %u), call %s; "
           "menu on screen %s (%s by %s); time %s; art shown %u, rx id %u size %lu have %u, starts %lu requests %lu "
           "resets %lu, failed %u",
           kMode[(int)_mode], kPage[_page % 4], _sel, item_label(_sel), v, alive(now_ms) ? "heard" : "not heard",
           _sensor_ms && now_ms - _sensor_ms < kSensorFreshMs ? "fresh" : "none", _tr_ms && now_ms - _tr_ms < 3000 ? "heard" : "none",
           _slot, phone_fresh(now_ms) ? kCall[_ph.call % 7] : "-", remote_open(now_ms) ? "open" : "closed",
           kScreenName[_ms.screen % 3], _ms.opened_by == ELC_CAN_MENU_BY_CONSOLE ? "console" : "touch",
           (_time_ms && (_time.flags & ELC_CAN_TIME_VALID)) ? "valid" : "none", _art_id, _art_rx.art_id(),
           (unsigned long)_art_rx.size(), _art_rx.chunks_have(), (unsigned long)_art_starts, (unsigned long)_art_reqs,
           (unsigned long)_art_resets, _art_failed_id);
}
