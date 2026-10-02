#include "console_ui.h"

#include <accessory_can.h>
#include <elc_sensor_units.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef WHEEL_CIRCUMFERENCE_M
#define WHEEL_CIRCUMFERENCE_M 2.1f  // same default as the displays
#endif

namespace {

constexpr uint32_t kSensorFreshMs = 1000;
constexpr uint32_t kOwnEchoMs = 2000;     // our own setting's echo does not toast
constexpr uint32_t kVolHoldMs = 1500;     // our volume wins over the bridge's stale echo
constexpr uint32_t kStatusMs = 3000;      // the audio card's STATUS is 1 Hz
constexpr uint32_t kArtQuietMs = 300;
constexpr uint8_t kArtMaxTries = 20;

const char *const kViewName[4] = {"Tachometer", "Clock", "Media", "Efficiency"};
const char *const kScreenName[3] = {"Left", "Center", "Right"};
// Display menu order Classic, Modern, Fallout; on the wire 0 Classic, 1 Fallout, 2 Modern.
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

// ---- frames and messages out ----

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

void ConsoleUi::log(const char *fmt, ...) {
  if (_ln == 4) {
    _lh = (uint8_t)((_lh + 1) % 4);
    _ln--;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(_log[(_lh + _ln) % 4], sizeof(_log[0]), fmt, ap);
  va_end(ap);
  _ln++;
}

bool ConsoleUi::pop_log(char *out, size_t n) {
  if (!_ln) return false;
  snprintf(out, n, "%s", _log[_lh]);
  _lh = (uint8_t)((_lh + 1) % 4);
  _ln--;
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

bool ConsoleUi::chan_send(uint16_t base, const ElcChWriter &w, const char *what) {
  if (!_ch) return false;
  const bool ok = _ch->send(base, w);
  log("chan %s%s", what, ok ? "" : " (not sent)");
  return ok;
}

/** AUDIO_CMD MEDIA to the audio card, which plays it or forwards it to the source (the radio). */
void ConsoleUi::media(uint8_t action, uint8_t value, const char *what) {
  const uint8_t s = sink();
  if (!s) return;
  ElcChWriter w;
  elc_audio_encode(ElcAudioMedia{s, action, value}, w);
  chan_send(ELC_CH_AUDIO_CMD, w, what);
}

void ConsoleUi::play_pause(uint32_t now_ms) {
  if (radio_is_source(now_ms)) {
    media(ELC_MEDIA_TOGGLE, 0, "MEDIA mute");
    return;
  }
  phone_cmd(ELC_CAN_PHONE_CMD_MEDIA, 0, "play/pause");  // the Bluetooth bridge (old frames)
  media(ELC_MEDIA_TOGGLE, 0, "MEDIA play/pause");       // and the audio card's own Bluetooth
}

void ConsoleUi::toast(const char *title, const char *value, uint32_t now_ms) {
  snprintf(_toast_title, sizeof(_toast_title), "%s", title);
  snprintf(_toast_value, sizeof(_toast_value), "%s", value);
  _toast_ms = now_ms ? now_ms : 1;
}

uint8_t ConsoleUi::sink() const { return _ch ? _ch->findNode(ELC_TYPE_AUDIO_SINK) : 0; }
uint8_t ConsoleUi::radio() const { return _ch ? _ch->findNode(ELC_TYPE_RADIO) : 0; }

uint8_t ConsoleUi::source(uint32_t now_ms) const {
  if (!_st_ms || now_ms - _st_ms > kStatusMs) return ELC_SRC_NONE;
  return _st.source;
}

const char *ConsoleUi::source_name(uint8_t code) const {
  switch (code) {
    case ELC_SRC_BLUETOOTH: return "Bluetooth";
    case ELC_SRC_RADIO: return "Radio";
    case ELC_SRC_AUX: return "Aux in";
    default: return "None";
  }
}

/** What plays, in two lines: the radio's NOW PLAYING, or the Bluetooth title and artist. */
void ConsoleUi::now_text(char *l1, size_t n1, char *l2, size_t n2, uint32_t now_ms) const {
  l1[0] = l2[0] = '\0';
  if (radio_is_source(now_ms)) {
    if (_now_ms && now_ms - _now_ms < 5000 && _now_playing.source == ELC_SRC_RADIO) {
      snprintf(l1, n1, "%s", _now_playing.line1);
      snprintf(l2, n2, "%s", _now_playing.line2);
    } else {
      snprintf(l1, n1, "Radio");
    }
    return;
  }
  const char *np = _np.valid() ? _np.text() : "";
  const char *sep = strstr(np, " - ");
  if (sep) {
    snprintf(l1, n1, "%s", sep + 3);
    snprintf(l2, n2, "%.*s", (int)(sep - np), np);
  } else {
    snprintf(l1, n1, "%s", np);
  }
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
  if (!_kinds_set) {  // paired names: kind 0x10 + index
    _kinds_set = true;
    for (uint8_t i = 0; i < ELC_CAN_PHONE_MAX_PAIRED; i++) _ph_paired[i].set_kind((uint8_t)(ELC_CAN_PHONE_TEXT_PAIRED + i));
  }
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
    if (!sink() && (!_vol_set || now_ms - _vol_set_ms > kVolHoldMs)) _vol = tr.volume > 15 ? 15 : tr.volume;
    return;
  }
  ElcCanPhoneStatus ps;
  if (elc_can_decode_phone_status(f, &slot, ps)) {
    if (_slot_known && slot != _slot && now_ms - _bridge_ms < 3000) return;
    _slot = slot;
    _slot_known = true;
    _bridge_ms = _ph_ms = now_ms;
    if (!(ps.flags & ELC_CAN_PHONE_CONFIRM)) _have_passkey = false;
    _ph = ps;
    return;
  }
  uint32_t key = 0;
  uint8_t left = 0;
  if (elc_can_decode_phone_pairing(f, &slot, &key, &left)) {
    if (_slot_known && slot != _slot) return;
    _passkey = key;
    _pair_left = left;
    _have_passkey = true;
    _pair_ms = now_ms;
    return;
  }
  const int s = elc_can_slot_of(f.id);
  if (s >= 0 && _slot_known && s != _slot) return;
  if (_np.feed(f) || _caller.feed(f) || _ph_connected.feed(f) || _ph_pairing.feed(f)) return;
  for (auto &d : _ph_paired) {
    if (d.feed(f)) return;
  }
  if (_art_rx.feed(f)) _art_rx_ms = now_ms;
}

void ConsoleUi::transfer(const ElcChTransfer &t, uint32_t now_ms) {
  _now = now_ms;
  if (t.base != ELC_CH_AUDIO_CMD) return;
  ElcAudioStatus st;
  ElcAudioNow np;
  if (elc_audio_decode(t, st)) {
    if (t.src != sink()) return;  // only the audio card (a sink announced as node type 5)
    const bool had = _st_ms && now_ms - _st_ms <= kStatusMs;
    if (had && st.source != _st.source && !(_src_req_ms && now_ms - _src_req_ms < kOwnEchoMs && st.source == _src_req)) {
      toast("SOURCE", source_name(st.source), now_ms);  // changed by someone else
    }
    _st = st;
    _st_ms = now_ms;
    if (!_vol_set || now_ms - _vol_set_ms > kVolHoldMs) _vol = (uint8_t)clampi((st.volume * 15 + 50) / 100, 0, 15);
  } else if (elc_audio_decode(t, np)) {
    if (t.src != _st.source_node && !(np.source == ELC_SRC_RADIO && t.src == radio())) return;
    _now_playing = np;
    _now_ms = now_ms;
  }
}

// ---- state ----

bool ConsoleUi::alive(uint32_t now_ms) const {
  return _booted && now_ms - _center_ms < kCenterGoneMs && now_ms - _boot_ms >= kBootMs;
}

bool ConsoleUi::call_shown(uint32_t now_ms) const { return phone_fresh(now_ms) && _ph.call != ELC_CAN_CALL_IDLE; }

bool ConsoleUi::pair_shown(uint32_t now_ms) const {
  return phone_fresh(now_ms) && (_ph.flags & ELC_CAN_PHONE_CONFIRM) && !call_shown(now_ms);
}

const char *ConsoleUi::phone_paired_name(uint8_t i) const {
  if (i >= ELC_CAN_PHONE_MAX_PAIRED || i >= _ph.paired) return "";
  const ElcCanPhoneTextDecoder &d = _ph_paired[i];
  return d.valid() && d.serial() == _ph.list_serial ? d.text() : "";
}

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

// ---- display settings (the Display page) ----

int ConsoleUi::setting_count(uint16_t id) const {
  switch (id) {
    case kRView: return 4;
    case kRTheme: return 3;
    case kRFollow: return 2;
    case kRIface: case kRMedia: return 3;
    case kRLayout: return 2;
    case kRContrast: return 3;
    default: return 0;  // brightness: 10..100 in 5s
  }
}

int ConsoleUi::setting_get(uint16_t id) const {
  switch (id) {
    case kRView: return _cl.center_subview & 3;
    case kRTheme: return theme_index(_cl.theme);
    case kRBright: return _bright_live >= 0 ? _bright_live : _cl.brightness;
    case kRFollow: return (_cl.flags & ELC_CAN_CLUSTER_DIM_WITH_LIGHTS) ? 0 : 1;  // On, Off
    case kRIface: return _cl.interface_screen % 3;
    case kRMedia: return _cl.media_screen % 3;
    case kRLayout: return (_cl.flags & ELC_CAN_CLUSTER_RIGHT_LAYOUT2) ? 1 : 0;
    case kRContrast: return (int)_contrast;
    default: return 0;
  }
}

void ConsoleUi::setting_set(uint16_t id, int v, uint32_t now_ms, bool undo) {
  const char *u = undo ? " (undo)" : "";
  char what[32];
  switch (id) {
    case kRView:
      snprintf(what, sizeof(what), "view%s", u);
      _cl.center_subview = (uint8_t)v;  // show the choice at once; the center's 0x210 confirms it
      setting(ELC_CAN_SET_CENTER_VIEW, (uint8_t)v, now_ms, what);
      break;
    case kRTheme:
      snprintf(what, sizeof(what), "theme%s", u);
      _cl.theme = kThemeWire[v];
      setting(ELC_CAN_SET_THEME, kThemeWire[v], now_ms, what);
      break;
    case kRBright:
      snprintf(what, sizeof(what), "brightness%s", u);
      _bright_live = v;
      setting(ELC_CAN_SET_BRIGHTNESS, (uint8_t)v, now_ms, what);
      break;
    case kRFollow:
      snprintf(what, sizeof(what), "dim-with-lights%s", u);
      _cl.flags = (uint8_t)(v == 0 ? (_cl.flags | ELC_CAN_CLUSTER_DIM_WITH_LIGHTS) : (_cl.flags & ~ELC_CAN_CLUSTER_DIM_WITH_LIGHTS));
      setting(ELC_CAN_SET_DIM_WITH_LIGHTS, v == 0 ? 1 : 0, now_ms, what);
      break;
    case kRIface:
      snprintf(what, sizeof(what), "menu-on%s", u);
      _cl.interface_screen = (uint8_t)v;
      setting(ELC_CAN_SET_INTERFACE_SCREEN, (uint8_t)v, now_ms, what);
      break;
    case kRMedia:
      snprintf(what, sizeof(what), "media-on%s", u);
      _cl.media_screen = (uint8_t)v;
      setting(ELC_CAN_SET_MEDIA_SCREEN, (uint8_t)v, now_ms, what);
      break;
    case kRLayout:
      snprintf(what, sizeof(what), "right-layout%s", u);
      _cl.flags = (uint8_t)(v ? (_cl.flags | ELC_CAN_CLUSTER_RIGHT_LAYOUT2) : (_cl.flags & ~ELC_CAN_CLUSTER_RIGHT_LAYOUT2));
      setting(ELC_CAN_SET_RIGHT_LAYOUT, (uint8_t)v, now_ms, what);
      break;
    case kRContrast:
      _contrast = (ConsoleContrast)v;
      _persist_dirty = true;
      break;
    default: break;
  }
}

void ConsoleUi::setting_text(uint16_t id, int v, char *out, size_t n) const {
  switch (id) {
    case kRView: snprintf(out, n, "%s", kViewName[v & 3]); break;
    case kRTheme: snprintf(out, n, "%s", kThemeName[v % 3]); break;
    case kRBright: snprintf(out, n, "%d%%", v); break;
    case kRFollow: snprintf(out, n, "%s", v == 0 ? "On" : "Off"); break;
    case kRIface: case kRMedia: snprintf(out, n, "%s", kScreenName[v % 3]); break;
    case kRLayout: snprintf(out, n, "%s", v ? "Fuel + lamps" : "Four gauges"); break;
    case kRContrast: snprintf(out, n, "%s", v == 0 ? "Auto" : (v == 1 ? "Bright" : "Night")); break;
    default: out[0] = '\0'; break;
  }
}

// ---- the menu ----

void ConsoleUi::open_menu(uint32_t now_ms) {
  (void)now_ms;
  _mode = ConsoleMode::Menu;
  _depth = 0;
  _stack[0] = Level();
  _advanced = false;
  build_rows();
}

void ConsoleUi::close_menu() {
  _mode = ConsoleMode::Drive;
  _depth = 0;
  _stack[0] = Level();
  accessory_can_view(0, ELC_UI_TOP);
}

void ConsoleUi::push(uint8_t page, uint8_t node, uint8_t arg) {
  if (_depth + 1 >= (int)(sizeof(_stack) / sizeof(_stack[0]))) return;
  _depth++;
  _stack[_depth] = Level();
  _stack[_depth].page = page;
  _stack[_depth].node = node;
  _stack[_depth].arg = arg;
  build_rows();
  for (int i = 0; i < _nrows; i++) {  // start on the first row that does something
    if (_rows[i].kind != ConsoleRow::Info) {
      _stack[_depth].sel = i;
      break;
    }
  }
}

void ConsoleUi::pop() {
  if (_depth == 0) {
    close_menu();
    return;
  }
  _depth--;
  build_rows();
}

void ConsoleUi::row_add(uint8_t kind, uint16_t id, uint8_t arg, const char *label, const char *value, bool dim) {
  if (_nrows >= (int)(sizeof(_rows) / sizeof(_rows[0]))) return;
  ConsoleRow &r = _rows[_nrows++];
  r.kind = kind;
  r.id = id;
  r.arg = arg;
  r.dim = dim;
  snprintf(r.label, sizeof(r.label), "%s", label);
  snprintf(r.value, sizeof(r.value), "%s", value ? value : "");
}

int ConsoleUi::menu_rows(const ConsoleRow **rows) {
  if (_mode == ConsoleMode::Drive) return 0;
  build_rows();
  *rows = _rows;
  return _nrows;
}

void ConsoleUi::build_rows() {
  _nrows = 0;
  const Level &L = _stack[_depth];
  const uint32_t now = _now;
  char v[40];
  switch (L.page) {
    case kMHome: {
      const uint8_t src = source(now);
      row_add(ConsoleRow::Sub, kRSource, 0, "Source", sink() ? source_name(src) : "No audio card", !sink());
      if (radio()) {
        char l1[40], l2[8];
        if (src == ELC_SRC_RADIO) now_text(l1, sizeof(l1), l2, sizeof(l2), now);
        else snprintf(l1, sizeof(l1), "Not playing");
        row_add(ConsoleRow::Sub, kRRadio, 0, "Radio", l1);
      }
      const char *ph = !phone_fresh(now)                              ? "Bridge not heard"
                       : (_ph.flags & ELC_CAN_PHONE_CONNECTED)        ? (_ph_connected.valid() && _ph_connected.text()[0]
                                                                            ? _ph_connected.text() : "Connected")
                                                                      : "Not connected";
      row_add(ConsoleRow::Sub, kRPhone, 0, "Phone", ph);
      uint8_t nodes[kAccMaxNodes];
      const int na = accessory_can_nodes(nodes, kAccMaxNodes);
      snprintf(v, sizeof(v), "%d", na);
      row_add(ConsoleRow::Sub, kRAccessories, 0, "Accessories", na ? v : "None");
      row_add(ConsoleRow::Sub, kRDisplay, 0, "Display", kThemeName[theme_index(_cl.theme)]);
      snprintf(v, sizeof(v), "%.1f mi", trip_mi());
      row_add(ConsoleRow::Sub, kRTrip, 0, "Trip A", v);
      row_add(ConsoleRow::Sub, kRService, 0, "Service", "");
      break;
    }
    case kMSource: {
      if (!sink()) {
        row_add(ConsoleRow::Info, kRNone, 0, "No audio card heard", "", true);
        break;
      }
      const uint8_t cur = source(now);
      row_add(ConsoleRow::Action, kRSrcPick, ELC_SRC_BLUETOOTH, "Bluetooth", cur == ELC_SRC_BLUETOOTH ? "Playing" : "");
      if (radio()) row_add(ConsoleRow::Action, kRSrcPick, ELC_SRC_RADIO, "Radio", cur == ELC_SRC_RADIO ? "Playing" : "");
      break;
    }
    case kMPhone: {
      if (!phone_fresh(now)) {
        row_add(ConsoleRow::Info, kRNone, 0, "Audio bridge not heard", "", true);
        break;
      }
      const bool conn = _ph.flags & ELC_CAN_PHONE_CONNECTED;
      const char *nm = _ph_connected.valid() && _ph_connected.text()[0] ? _ph_connected.text() : "Phone";
      row_add(ConsoleRow::Info, kRPhStatus, 0, conn ? nm : "No phone connected",
              conn ? ((_ph.flags & ELC_CAN_PHONE_HFP) ? "Hands-free" : "Audio only") : "", !conn);
      if (_ph.flags & ELC_CAN_PHONE_PAIRING) {
        if (_pair_left != 0xFF && _pair_ms) {
          const uint32_t gone = (now - _pair_ms) / 1000;
          const int left = _pair_left > gone ? (int)(_pair_left - gone) : 0;
          snprintf(v, sizeof(v), "%d:%02d", left / 60, left % 60);
        } else {
          snprintf(v, sizeof(v), "On");
        }
        row_add(ConsoleRow::Action, kRPhStop, 0, "Stop pairing", v);
      } else {
        row_add(ConsoleRow::Action, kRPhPair, 0, "Pair new phone", "");
      }
      if (_ph.flags & ELC_CAN_PHONE_HFP) {
        row_add(ConsoleRow::Action, kRPhVoice, 0, "Voice assistant", (_ph.flags & ELC_CAN_PHONE_VOICE) ? "On" : "Off");
        row_add(ConsoleRow::Action, kRPhRedial, 0, "Redial", "");
      }
      for (uint8_t i = 0; i < _ph.paired && i < ELC_CAN_PHONE_MAX_PAIRED; i++) {
        const char *pn = phone_paired_name(i);
        char lb[24];
        if (pn[0]) snprintf(lb, sizeof(lb), "%s", pn);
        else snprintf(lb, sizeof(lb), "Phone %u", (unsigned)i + 1);
        row_add(ConsoleRow::Sub, kRPhPaired, i, lb, conn && _ph.connected_index == i ? "Connected" : "Paired");
      }
      break;
    }
    case kMPhoneOne: {
      const uint8_t i = L.arg;
      if (i >= _ph.paired) {
        row_add(ConsoleRow::Info, kRNone, 0, "Not in the list any more", "", true);
        break;
      }
      const bool on = (_ph.flags & ELC_CAN_PHONE_CONNECTED) && _ph.connected_index == i;
      if (on) row_add(ConsoleRow::Action, kRPhDisconnect, i, "Disconnect", "");
      else row_add(ConsoleRow::Action, kRPhConnect, i, "Connect", "");
      row_add(ConsoleRow::Action, kRPhForget, i, "Forget", "");
      break;
    }
    case kMDisplay: {
      static const uint16_t kIds[8] = {kRView, kRTheme, kRBright, kRFollow, kRIface, kRMedia, kRLayout, kRContrast};
      static const char *const kLabels[8] = {"Center view", "Theme", "Brightness", "Dim with lights", "Menu on",
                                             "Media on", "Right layout", "OLED contrast"};
      for (int k = 0; k < 8; k++) {
        const int val = (_mode == ConsoleMode::Edit && _edit_id == kIds[k]) ? _edit_val : setting_get(kIds[k]);
        setting_text(kIds[k], val, v, sizeof(v));
        const bool classic_only = kIds[k] == kRLayout && _cl.theme != 0;
        row_add(ConsoleRow::Setting, kIds[k], 0, kLabels[k], classic_only ? "Classic only" : v, classic_only);
      }
      break;
    }
    case kMTrip:
      snprintf(v, sizeof(v), "%.1f mi", trip_mi());
      row_add(ConsoleRow::Info, kRNone, 0, "Trip A", v);
      row_add(ConsoleRow::Action, kRTripReset, 0, "Reset Trip A", "", !_s.odo_valid);
      break;
    case kMService: {
      const bool fresh = _sensor_ms && now - _sensor_ms < kSensorFreshMs;
      row_add(ConsoleRow::Info, kRNone, 0, "Center", alive(now) ? "Heard" : "Not heard", !alive(now));
      row_add(ConsoleRow::Info, kRNone, 0, "Sensors", fresh ? "Fresh" : "No data", !fresh);
      snprintf(v, sizeof(v), "Node 0x%02X", (unsigned)(ELC_CAN_NODE_SLOT0 + _slot));
      row_add(ConsoleRow::Info, kRNone, 0, "Audio bridge", bridge_heard(now) ? v : "Not heard", !bridge_heard(now));
      if (sink()) snprintf(v, sizeof(v), "0x%02X \xC2\xB7 %s", (unsigned)sink(), source_name(source(now)));
      row_add(ConsoleRow::Info, kRNone, 0, "Audio card", sink() ? v : "Not heard", !sink());
      if (radio()) snprintf(v, sizeof(v), "Node 0x%02X", (unsigned)radio());
      row_add(ConsoleRow::Info, kRNone, 0, "Radio", radio() ? v : "Not heard", !radio());
      int nodes = 0;
      for (uint8_t i = 1; _ch && i < 32; i++) nodes += _ch->nodeInfo(i).announced;
      snprintf(v, sizeof(v), "%d announced", nodes);
      row_add(ConsoleRow::Info, kRNone, 0, "Channel nodes", _ch ? v : "Off", !_ch);
      break;
    }
    case kMAccessories: {
      uint8_t nodes[kAccMaxNodes];
      const int n = accessory_can_nodes(nodes, kAccMaxNodes);
      if (!n) row_add(ConsoleRow::Info, kRNone, 0, "No accessory pages yet", "", true);
      for (int i = 0; i < n; i++) row_add(ConsoleRow::Sub, kRAccNode, nodes[i], accessory_can_name(nodes[i]), "");
      break;
    }
    case kMAccPage: {
      const AccItem *items[32];
      const int n = accessory_can_items(L.node, L.arg, _advanced, items, 31);
      if (!n) row_add(ConsoleRow::Info, kRNone, 0, "Nothing on this page yet", "", true);
      for (int i = 0; i < n; i++) {
        const AccItem &it = *items[i];
        char val[40];
        const bool editing = _mode == ConsoleMode::Edit && _edit_node == L.node && _edit_item == it.d.item;
        if (editing) {
          char num[24];
          elc_ui_format(_edit_val, it.d.exponent, num, sizeof(num));
          snprintf(val, sizeof(val), it.d.unit[0] ? "%s %s" : "%s", num, it.d.unit);
        } else if (it.pending) {
          snprintf(val, sizeof(val), "\xE2\x80\xA6");
        } else if (it.result_ms && it.result && now - it.result_ms < 4000) {
          snprintf(val, sizeof(val), "%s", it.result == 0xFF ? "No answer" : "Refused");
        } else {
          accessory_can_value_text(it, val, sizeof(val));
        }
        const bool ro = (it.d.flags & ELC_UI_F_RO) || it.d.kind == ELC_UI_READOUT || it.d.kind == ELC_UI_TEXTOUT;
        const uint8_t kind = it.d.kind == ELC_UI_GROUP ? ConsoleRow::Sub : (ro ? ConsoleRow::Info : ConsoleRow::Action);
        row_add(kind, kRAccItem, it.d.item, it.d.label, it.d.kind == ELC_UI_GROUP ? "" : val);
      }
      if (L.arg == ELC_UI_TOP) {
        const AccItem *all[32];
        if (accessory_can_items(L.node, L.arg, true, all, 31) > n || _advanced) {
          row_add(ConsoleRow::Action, kRAdvanced, 0, "Show advanced", _advanced ? "On" : "Off");
        }
      }
      break;
    }
    case kMAccChoice: {
      const AccItem *it = accessory_can_item(L.node, L.arg);
      if (!it) break;
      const int n = it->d.option_count();
      for (int k = 0; k < n && k < 30; k++) {
        char opt[33];
        if (!it->d.option(k, opt, sizeof(opt))) break;
        row_add(ConsoleRow::Action, kRAccOpt, (uint8_t)k, opt, it->has_value && it->value == k ? "Current" : "");
      }
      break;
    }
    case kMConfirm:
      row_add(ConsoleRow::Info, kRNone, 0, _cf_question, "");
      row_add(ConsoleRow::Action, kRNo, 0, "Cancel", "");
      row_add(ConsoleRow::Action, kRYes, 0, "OK", "");
      break;
  }
  Level &M = _stack[_depth];
  if (M.sel >= _nrows) M.sel = _nrows ? _nrows - 1 : 0;
  if (M.sel < 0) M.sel = 0;
}

void ConsoleUi::menu_move(int steps) {
  Level &M = _stack[_depth];
  M.sel = clampi(M.sel + steps, 0, _nrows ? _nrows - 1 : 0);
}

void ConsoleUi::confirm(uint16_t what, uint8_t node, uint8_t arg, int32_t value, const char *question) {
  _cf_what = what;
  _cf_node = node;
  _cf_arg = arg;
  _cf_value = value;
  snprintf(_cf_question, sizeof(_cf_question), "%s", question);
  push(kMConfirm);
  _stack[_depth].sel = 1;  // on Cancel
}

void ConsoleUi::do_confirmed(uint32_t now_ms) {
  switch (_cf_what) {
    case kRPhForget: phone_cmd(ELC_CAN_PHONE_CMD_FORGET, _cf_arg, "forget"); break;
    case kRTripReset:
      if (_s.odo_valid) {
        _trip_base = _s.odo_tenths;
        _trip_set = true;
        _persist_dirty = true;
        toast("TRIP A", "Reset", now_ms);
      }
      break;
    case kRAccItem: accessory_can_set(_cf_node, _cf_arg, _cf_value); break;
    default: break;
  }
}

void ConsoleUi::menu_click(uint32_t now_ms) {
  build_rows();
  const Level L = _stack[_depth];
  if (L.sel >= _nrows) return;
  const ConsoleRow r = _rows[L.sel];
  switch (r.id) {
    case kRSource: push(kMSource); return;
    case kRRadio: push(kMAccPage, radio(), ELC_UI_TOP); return;
    case kRPhone: push(kMPhone); return;
    case kRAccessories: push(kMAccessories); return;
    case kRDisplay: push(kMDisplay); return;
    case kRTrip: push(kMTrip); return;
    case kRService: push(kMService); return;
    case kRPhPaired: push(kMPhoneOne, 0, r.arg); return;
    case kRAccNode: push(kMAccPage, r.arg, ELC_UI_TOP); return;
    case kRSrcPick: {
      const uint8_t s = sink();
      if (!s) return;
      ElcChWriter w;
      elc_audio_encode(ElcAudioSource{s, r.arg}, w);
      _src_req = r.arg;
      _src_req_ms = now_ms;
      chan_send(ELC_CH_AUDIO_CMD, w, r.arg == ELC_SRC_RADIO ? "SOURCE radio" : "SOURCE bluetooth");
      toast("SOURCE", source_name(r.arg), now_ms);
      close_menu();
      return;
    }
    case kRPhPair: phone_cmd(ELC_CAN_PHONE_CMD_PAIRING, 1, "pairing on"); return;
    case kRPhStop: phone_cmd(ELC_CAN_PHONE_CMD_PAIRING, 0, "pairing off"); return;
    case kRPhVoice: phone_cmd(ELC_CAN_PHONE_CMD_VOICE, (_ph.flags & ELC_CAN_PHONE_VOICE) ? 0 : 1, "voice"); return;
    case kRPhRedial: phone_cmd(ELC_CAN_PHONE_CMD_REDIAL, 0, "redial"); close_menu(); return;
    case kRPhConnect: phone_cmd(ELC_CAN_PHONE_CMD_CONNECT, r.arg, "connect"); pop(); return;
    case kRPhDisconnect: phone_cmd(ELC_CAN_PHONE_CMD_DISCONNECT, 0, "disconnect"); pop(); return;
    case kRPhForget: {
      char q[48];
      const char *pn = phone_paired_name(r.arg);
      snprintf(q, sizeof(q), "Forget %s?", pn[0] ? pn : "this phone");
      confirm(kRPhForget, 0, r.arg, 0, q);
      return;
    }
    case kRTripReset:
      if (_s.odo_valid) confirm(kRTripReset, 0, 0, 0, "Reset Trip A?");
      return;
    case kRAdvanced: _advanced = !_advanced; build_rows(); return;
    case kRAccItem: {
      const AccItem *it = accessory_can_item(L.node, r.arg);
      if (!it) return;
      if (it->d.kind == ELC_UI_GROUP) {
        push(kMAccPage, L.node, it->d.item);
        return;
      }
      if (it->d.flags & ELC_UI_F_RO) return;
      int32_t v = 0;
      switch (it->d.kind) {
        case ELC_UI_TOGGLE: v = it->has_value && it->value ? 0 : 1; break;
        case ELC_UI_ACTION: v = 1; break;
        case ELC_UI_CHOICE: push(kMAccChoice, L.node, it->d.item); return;
        case ELC_UI_NUMBER:
          _mode = ConsoleMode::Edit;
          _edit_id = kRAccItem;
          _edit_node = L.node;
          _edit_item = it->d.item;
          _edit_val = it->has_value ? it->value : it->d.min;
          if (it->d.max > it->d.min) _edit_val = clampi(_edit_val, it->d.min, it->d.max);
          build_rows();
          return;
        default: return;
      }
      if (it->d.flags & ELC_UI_F_CONFIRM) {
        char q[48];
        snprintf(q, sizeof(q), "%s?", it->d.label);
        confirm(kRAccItem, L.node, it->d.item, v, q);
      } else {
        accessory_can_set(L.node, it->d.item, v);
      }
      return;
    }
    case kRAccOpt: {
      const AccItem *it = accessory_can_item(L.node, L.arg);
      if (!it) return;
      if (it->d.flags & ELC_UI_F_CONFIRM) {
        char q[48], opt[33] = "";
        it->d.option(r.arg, opt, sizeof(opt));
        snprintf(q, sizeof(q), "%s: %s?", it->d.label, opt);
        pop();
        confirm(kRAccItem, L.node, it->d.item, r.arg, q);
      } else {
        accessory_can_set(L.node, it->d.item, r.arg);
        pop();
      }
      return;
    }
    case kRYes: {
      const uint16_t what = _cf_what;
      pop();
      do_confirmed(now_ms);
      if (what == kRPhForget) pop();  // the phone's page goes with it
      return;
    }
    case kRNo: pop(); return;
    default: break;
  }
  if (r.kind == ConsoleRow::Setting && !r.dim) {
    _mode = ConsoleMode::Edit;
    _edit_id = r.id;
    _edit_val = _edit_orig = setting_get(r.id);
    build_rows();
  }
}

void ConsoleUi::edit_turn(int steps, uint32_t now_ms) {
  if (_edit_id == kRAccItem) {
    const AccItem *it = accessory_can_item(_edit_node, _edit_item);
    if (!it) return;
    const int32_t step = it->d.step > 0 ? it->d.step : 1;
    int32_t v = _edit_val + steps * step;
    if (it->d.max > it->d.min) v = clampi(v, it->d.min, it->d.max);
    _edit_val = v;
  } else if (_edit_id == kRBright) {
    const int v = clampi(_edit_val + steps * 5, 10, 100);
    if (v != _edit_val) setting_set(kRBright, _edit_val = v, now_ms);
  } else if (setting_count(_edit_id)) {
    const int v = clampi(_edit_val + steps, 0, setting_count(_edit_id) - 1);
    if (v != _edit_val) setting_set(_edit_id, _edit_val = v, now_ms);  // live: the screens follow as it turns
  }
  build_rows();
}

void ConsoleUi::edit_done(uint32_t now_ms) {
  (void)now_ms;
  if (_edit_id == kRAccItem) accessory_can_set(_edit_node, _edit_item, _edit_val);
  _mode = ConsoleMode::Menu;
  _edit_id = 0;
  build_rows();
}

void ConsoleUi::edit_cancel(uint32_t now_ms) {
  if (_edit_id != kRAccItem && _edit_val != _edit_orig) setting_set(_edit_id, _edit_orig, now_ms, true);
  _mode = ConsoleMode::Menu;
  _edit_id = 0;
  build_rows();
}

// ---- the preset list ----

bool ConsoleUi::preset_khz(int i, uint32_t *khz) const {
  const uint8_t r = radio();
  if (!r || i < 0 || i >= kPresets) return false;
  const AccItem *it = accessory_can_item(r, (uint8_t)(kRadioPresetItem0 + i));
  if (!it || !it->has_value || it->value <= 0) return false;
  if (khz) *khz = (uint32_t)it->value;
  return true;
}

void ConsoleUi::presets_open(uint32_t now_ms) {
  _pre_open = true;
  _pre_ms = now_ms;
  // start on the preset playing now (NOW PLAYING names it), else the first slot
  const bool np = _now_ms && now_ms - _now_ms < 5000 && _now_playing.source == ELC_SRC_RADIO;
  _pre_sel = np && _now_playing.preset >= 1 && _now_playing.preset <= kPresets ? _now_playing.preset - 1 : 0;
}

// ---- input ----

void ConsoleUi::page_step(int dir, uint32_t now_ms) {
  _page_from = _page;
  _page = (uint8_t)((_page + dir + kPageCount) % kPageCount);
  _page_ms = now_ms;
  _page_dir = (int8_t)dir;
}

/** Left knob turn: volume at rest (the call's volume in a call), the selection in the menu, the value in an editor. */
void ConsoleUi::left_turn(int steps, uint32_t now_ms) {
  _now = now_ms;
  if (!steps || !alive(now_ms)) return;
  touched(now_ms);
  if (_mode == ConsoleMode::Edit) {
    edit_turn(steps, now_ms);
    return;
  }
  if (_mode == ConsoleMode::Menu && !call_shown(now_ms)) {
    menu_move(steps);
    return;
  }
  if (!_vol_ms || now_ms - _vol_ms >= kVolumeShowMs) _vol_shown_ms = now_ms;  // the bar was hidden: it slides in
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
  phone_cmd(ELC_CAN_PHONE_CMD_MEDIA_VOLUME, (uint8_t)v, "volume");  // the Bluetooth bridge (0..15)
  if (const uint8_t s = sink()) {                                   // the audio card (0..100)
    ElcChWriter w;
    elc_audio_encode(ElcAudioVolume{s, 0, (int8_t)((v * 100 + 7) / 15)}, w);
    chan_send(ELC_CH_AUDIO_CMD, w, "VOLUME");
  }
}

/** Left knob click: the next source at rest (mic mute in a call), select in the menu, confirm in an editor. */
void ConsoleUi::left_click(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms)) return;
  touched(now_ms);
  if (call_shown(now_ms)) {
    if (_ph.call == ELC_CAN_CALL_ACTIVE || _ph.call == ELC_CAN_CALL_HELD || _ph.call == ELC_CAN_CALL_WAITING) {
      phone_cmd(ELC_CAN_PHONE_CMD_MIC_MUTE, (_ph.flags & ELC_CAN_PHONE_MIC_MUTED) ? 0 : 1, "mute");
    }
    return;
  }
  if (_mode == ConsoleMode::Edit) {
    edit_done(now_ms);
    return;
  }
  if (_mode == ConsoleMode::Menu) {
    menu_click(now_ms);
    return;
  }
  presets_close("left click");
  const uint8_t s = sink();
  if (!s) {
    toast("SOURCE", "No audio card", now_ms);
    return;
  }
  // the next source, shown at once; STATUS confirms it
  const uint8_t cur = source(now_ms);
  const uint8_t next = cur == ELC_SRC_BLUETOOTH && radio() ? ELC_SRC_RADIO : ELC_SRC_BLUETOOTH;
  ElcChWriter w;
  elc_audio_encode(ElcAudioSource{s, ELC_SRC_NONE}, w);
  _src_req = next;
  _src_req_ms = now_ms;
  chan_send(ELC_CH_AUDIO_CMD, w, "SOURCE next");
  toast("SOURCE", source_name(next), now_ms);
}

/** Left knob hold: open the menu (close it when open). */
void ConsoleUi::left_hold(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms) || call_shown(now_ms) || pair_shown(now_ms)) return;
  touched(now_ms);
  presets_close("left hold");
  if (_mode == ConsoleMode::Drive) open_menu(now_ms);
  else close_menu();
}

/** Right knob turn: tune (radio) or skip tracks (Bluetooth) at rest; the selection or value in the menu. */
void ConsoleUi::right_turn(int steps, uint32_t now_ms) {
  _now = now_ms;
  if (!steps || !alive(now_ms) || call_shown(now_ms)) return;
  touched(now_ms);
  if (_mode == ConsoleMode::Edit) {
    edit_turn(steps, now_ms);
    return;
  }
  if (_mode == ConsoleMode::Menu) {
    menu_move(steps);
    return;
  }
  if (_pre_open) {  // the preset list: move along it, tuning to each stored one as it lands
    _pre_ms = now_ms;
    _pre_sel = ((_pre_sel + steps) % kPresets + kPresets) % kPresets;
    if (preset_khz(_pre_sel, nullptr)) media(ELC_MEDIA_PRESET_RECALL, (uint8_t)(_pre_sel + 1), "MEDIA recall preset");
    return;
  }
  const int n = clampi(steps < 0 ? -steps : steps, 1, 5);
  for (int i = 0; i < n; i++) {
    if (radio_is_source(now_ms)) {
      media(steps > 0 ? ELC_MEDIA_TUNE_UP : ELC_MEDIA_TUNE_DOWN, 0, steps > 0 ? "MEDIA tune up" : "MEDIA tune down");
    } else {
      phone_cmd(ELC_CAN_PHONE_CMD_MEDIA, steps > 0 ? 1 : 2, steps > 0 ? "next" : "previous");
      media(steps > 0 ? ELC_MEDIA_NEXT : ELC_MEDIA_PREV, 0, steps > 0 ? "MEDIA next" : "MEDIA previous");
    }
  }
}

/** Right knob click: the next preset (radio) or the voice assistant (Bluetooth); select in the menu. */
void ConsoleUi::right_click(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms) || call_shown(now_ms)) return;
  touched(now_ms);
  if (_mode == ConsoleMode::Edit) {
    edit_done(now_ms);
    return;
  }
  if (_mode == ConsoleMode::Menu) {
    menu_click(now_ms);
    return;
  }
  if (radio_is_source(now_ms)) {
    if (_pre_open) presets_close("right click");
    else presets_open(now_ms);
  } else if (phone_fresh(now_ms) && (_ph.flags & ELC_CAN_PHONE_HFP)) {
    const bool on = !(_ph.flags & ELC_CAN_PHONE_VOICE);
    phone_cmd(ELC_CAN_PHONE_CMD_VOICE, on ? 1 : 0, "voice");
    toast("VOICE", on ? "Listening" : "Off", now_ms);
  }
}

/** Right knob hold: seek up (radio) or redial (Bluetooth). */
void ConsoleUi::right_hold(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms) || call_shown(now_ms) || _mode != ConsoleMode::Drive) return;
  touched(now_ms);
  if (radio_is_source(now_ms)) {
    media(ELC_MEDIA_SEEK_UP, 0, "MEDIA seek up");
    toast("RADIO", "Seeking", now_ms);
  } else if (phone_fresh(now_ms) && (_ph.flags & ELC_CAN_PHONE_HFP)) {
    phone_cmd(ELC_CAN_PHONE_CMD_REDIAL, 0, "redial");
    toast("PHONE", "Redial", now_ms);
  }
}

/** Left soft key: Decline / Ignore / End in a call, Reject a pairing, acknowledge a warning, Back in the menu (undo in
 *  an editor), play / pause (mute for the radio) at rest. */
void ConsoleUi::soft_left(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms)) return;
  touched(now_ms);
  if (call_shown(now_ms)) {
    if (_ph.call == ELC_CAN_CALL_WAITING) phone_cmd(ELC_CAN_PHONE_CMD_CALL_WAITING, 0, "ignore");
    else phone_cmd(ELC_CAN_PHONE_CMD_END, 0, _ph.call == ELC_CAN_CALL_INCOMING ? "decline" : "end");
    return;
  }
  if (pair_shown(now_ms)) {
    phone_cmd(ELC_CAN_PHONE_CMD_CONFIRM, 0, "reject pairing");
    return;
  }
  if (_mode == ConsoleMode::Edit) {
    edit_cancel(now_ms);
    return;
  }
  if (_mode == ConsoleMode::Menu) {
    pop();
    return;
  }
  static const char *const kIds[6] = {"nodata", "oil", "hot", "gen", "fuel", "odo"};
  if (const ConsoleAlert *a = alert(now_ms)) {
    for (int k = 0; k < 6; k++) {
      if (!strcmp(a->id, kIds[k])) _ack[k] = true;
    }
    return;
  }
  play_pause(now_ms);
}

/** Left soft key held: store the station as a preset (radio). */
void ConsoleUi::soft_left_hold(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms) || call_shown(now_ms) || _mode != ConsoleMode::Drive) return;
  touched(now_ms);
  if (!radio_is_source(now_ms)) return;
  if (_pre_open) {  // into the slot highlighted, over whatever it held
    _pre_ms = now_ms;
    media(ELC_MEDIA_PRESET_STORE, (uint8_t)(_pre_sel + 1), "MEDIA store preset");
    char v[16];
    snprintf(v, sizeof(v), "Stored in %d", _pre_sel + 1);
    toast("PRESET", v, now_ms);
    return;
  }
  media(ELC_MEDIA_PRESET_STORE, 0, "MEDIA store preset");  // the next free slot (else the oldest)
  toast("PRESET", "Stored", now_ms);
}

/** Right soft key: Answer / Mute / Swap in a call, Pair a phone, leave the menu, the next OLED page at rest. */
void ConsoleUi::soft_right(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms)) return;
  touched(now_ms);
  if (call_shown(now_ms)) {
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
  if (pair_shown(now_ms)) {
    phone_cmd(ELC_CAN_PHONE_CMD_CONFIRM, 1, "accept pairing");
    return;
  }
  if (_mode == ConsoleMode::Edit) edit_done(now_ms);
  if (_mode == ConsoleMode::Menu) {
    close_menu();
    return;
  }
  if (_pre_open) {
    presets_close("right soft");
    return;
  }
  page_step(1, now_ms);
}

/** Right soft key held: the next view on the center screen. */
void ConsoleUi::soft_right_hold(uint32_t now_ms) {
  _now = now_ms;
  if (!alive(now_ms) || call_shown(now_ms) || _mode != ConsoleMode::Drive) return;
  touched(now_ms);
  if (_cs_ms && (_cs.flags & ELC_CAN_STATUS_REVERSE)) return;
  const uint8_t v = next_view();
  _view_req = v;
  _view_req_ms = now_ms;
  setting(ELC_CAN_SET_CENTER_VIEW, v, now_ms, "view");
  toast("VIEW", kViewName[v & 3], now_ms);
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
  if (_mode == ConsoleMode::Edit && !alive(now_ms)) {
    _mode = ConsoleMode::Menu;
    _edit_id = 0;
  }
  if (_pre_open) {
    // signed: an input stamped a little after this pass's clock (the serial console) is not "long ago"
    if ((int32_t)(now_ms - _pre_ms) > (int32_t)kPresetsIdleMs) presets_close("idle");
    else if (!radio_is_source(now_ms)) presets_close("radio not the source");
    else if (_mode != ConsoleMode::Drive) presets_close("menu");
    else if (call_shown(now_ms)) presets_close("call");
  }
  if (_mode != ConsoleMode::Drive) {
    build_rows();
    // keep an accessory page's live items coming while it shows (UI VIEW)
    const Level &L = _stack[_depth];
    if (L.page == kMAccPage || L.page == kMAccChoice) accessory_can_view(L.node, L.page == kMAccPage ? L.arg : ELC_UI_TOP);
    else accessory_can_view(0, ELC_UI_TOP);
  }
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
  if ((call_shown(now_ms) && (_ph.call == ELC_CAN_CALL_INCOMING || _ph.call == ELC_CAN_CALL_WAITING)) ||
      pair_shown(now_ms)) {
    const uint32_t p = now_ms % 1000;
    return (p < 500 ? p : 1000 - p) / 500.0f;  // pulse while it rings or a code waits
  }
  return base;
}

void ConsoleUi::describe(char *out, size_t n, uint32_t now_ms) const {
  static const char *const kMode[3] = {"drive", "menu", "edit"};
  static const char *const kPage[4] = {"media", "trip", "engine", "clock"};
  static const char *const kCall[7] = {"idle", "incoming", "dialing", "alerting", "active", "waiting", "held"};
  static const char *const kMenu[11] = {"home", "source", "phone", "phone-one", "display", "trip", "service",
                                        "accessories", "accessory", "choice", "confirm"};
  const Level &L = _stack[_depth];
  char l1[40], l2[72];
  now_text(l1, sizeof(l1), l2, sizeof(l2), now_ms);
  snprintf(out, n,
           "mode %s, page %s, presets %s %d (%lu ms idle, last closed: %s), menu %s (row %d); center %s, sensors %s, bridge %s (slot %u), call %s, pairing %s; "
           "audio card %s, source %s, playing '%s' '%s'; radio %s; time %s; art shown %u, rx id %u, starts %lu "
           "requests %lu resets %lu, failed %u",
           kMode[(int)_mode % 3], kPage[_page % 4], _pre_open ? "open" : "closed", _pre_sel + 1,
           (unsigned long)(now_ms - _pre_ms), _pre_why, kMenu[L.page % 11], L.sel, alive(now_ms) ? "heard" : "not heard",
           _sensor_ms && now_ms - _sensor_ms < kSensorFreshMs ? "fresh" : "none", bridge_heard(now_ms) ? "heard" : "none",
           _slot, phone_fresh(now_ms) ? kCall[_ph.call % 7] : "-", pair_shown(now_ms) ? "code waiting" : "-",
           sink() ? "heard" : "none", source_name(source(now_ms)), l1, l2, radio() ? "heard" : "none",
           (_time_ms && (_time.flags & ELC_CAN_TIME_VALID)) ? "valid" : "none", _art_id, _art_rx.art_id(),
           (unsigned long)_art_starts, (unsigned long)_art_reqs, (unsigned long)_art_resets, _art_failed_id);
}
