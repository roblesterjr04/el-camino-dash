#include "phone_can.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <elc_fonts.h>
#include <elc_gauges.h>
#include <p4_screen.h>
#include <ui_menu.h>
#include <ui_theme.h>

#if defined(ARDUINO)
#include <Arduino.h>
#else
extern "C" uint32_t millis(void);
#endif

// Material Icons code points as UTF-8 (both are in elc_icons_56)
#define IC_PHONE "\xEE\x8C\xAC"
#define IC_BLUETOOTH "\xEE\x86\xA7"

namespace {

constexpr uint32_t kStaleMs = 3000;  // the bridge sends status every second: three missed = gone

bool (*s_send)(const ElcCanFrame &) = nullptr;
uint8_t s_node = 0;

// ---- bridge state (one bridge expected; the newest status wins)
ElcCanBridgeSeen s_bridges;
ElcCanPhoneStatus s_st;
uint32_t s_st_ms = 0;
bool s_have = false;
uint8_t s_slot = 0;
ElcCanPhoneTextDecoder s_paired[ELC_CAN_PHONE_MAX_PAIRED] = {  // paired names: kind 0x10 + i
    ElcCanPhoneTextDecoder(ELC_CAN_PHONE_TEXT_PAIRED + 0), ElcCanPhoneTextDecoder(ELC_CAN_PHONE_TEXT_PAIRED + 1),
    ElcCanPhoneTextDecoder(ELC_CAN_PHONE_TEXT_PAIRED + 2), ElcCanPhoneTextDecoder(ELC_CAN_PHONE_TEXT_PAIRED + 3),
    ElcCanPhoneTextDecoder(ELC_CAN_PHONE_TEXT_PAIRED + 4), ElcCanPhoneTextDecoder(ELC_CAN_PHONE_TEXT_PAIRED + 5),
    ElcCanPhoneTextDecoder(ELC_CAN_PHONE_TEXT_PAIRED + 6), ElcCanPhoneTextDecoder(ELC_CAN_PHONE_TEXT_PAIRED + 7)};
static_assert(ELC_CAN_PHONE_MAX_PAIRED == 8, "one decoder per paired phone");
uint8_t s_pair_left = 255;  // pairing window seconds left (PHONE_PAIRING)
ElcCanPhoneTextDecoder s_caller(ELC_CAN_PHONE_TEXT_CALLER);
ElcCanPhoneTextDecoder s_name(ELC_CAN_PHONE_TEXT_CONNECTED);
ElcCanPhoneTextDecoder s_pair_name(ELC_CAN_PHONE_TEXT_PAIRING);
uint32_t s_pair_name_ms = 0;
uint32_t s_confirm_since_ms = 0;  // when CONFIRM last came on: an older pairing name is from another phone
uint32_t s_passkey = 0;
uint32_t s_passkey_ms = 0;
bool s_have_passkey = false;

// ---- bench demo: synthetic bridge frames, fed locally; real bridge frames are ignored meanwhile
enum class Demo : uint8_t { Off, Idle, Discover, Incoming, Active, Waiting, Held, Pair };
Demo s_demo = Demo::Off;
bool s_demo_muted = false, s_demo_on_phone = false;
uint8_t s_demo_conn = 0;  // connected index in the demo's paired list (ELC_CAN_PHONE_NONE = none)
bool s_demo_feeding = false;
uint32_t s_demo_start_ms = 0, s_demo_sent_ms = 0;

bool heard(uint32_t now) { return s_have && now - s_st_ms < kStaleMs; }

bool ingest(const ElcCanFrame &f) {
  const uint32_t now = millis();
  ElcCanPhoneStatus st;
  uint8_t slot = 0;
  if (elc_can_decode_phone_status(f, &slot, st)) {
    s_slot = slot;
    const bool was_confirm = heard(now) && (s_st.flags & ELC_CAN_PHONE_CONFIRM);
    if ((st.flags & ELC_CAN_PHONE_CONFIRM) && !was_confirm) {
      s_confirm_since_ms = now;
    }
    s_st = st;
    s_st_ms = now;
    s_have = true;
    return true;
  }
  uint32_t key = 0;
  uint8_t left = 255;
  if (elc_can_decode_phone_pairing(f, nullptr, &key, &left)) {
    s_pair_left = left;
    s_passkey = key;
    s_passkey_ms = now;
    s_have_passkey = true;
    return true;
  }
  const int text_slot = elc_can_slot_of(f.id);
  if (text_slot < 0 || f.id != elc_can_slot_id((uint8_t)text_slot, ELC_CAN_AUDIO_MSG_PHONE_TEXT)) {
    return false;
  }
  // Each decoder takes only its own kind.
  if (s_pair_name.feed(f)) s_pair_name_ms = now;
  s_caller.feed(f);
  s_name.feed(f);
  for (ElcCanPhoneTextDecoder &d : s_paired) d.feed(f);
  return true;
}

// ---------------------------------------------------------------- demo
const char *const kDemoPaired[] = {"Robert's iPhone", "Work Galaxy S23"};

void demo_feed(uint32_t now) {
  ElcCanPhoneStatus st;
  const bool conn = s_demo_conn != ELC_CAN_PHONE_NONE;
  st.flags = conn ? (ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP) : 0;
  st.signal = conn ? 4 : ELC_CAN_PHONE_NONE;
  st.battery = conn ? 3 : ELC_CAN_PHONE_NONE;
  st.caller_serial = 5;
  st.list_serial = 1;
  st.paired = 2;
  st.connected_index = s_demo_conn;
  st.call_volume = 9;
  const uint32_t secs = (now - s_demo_start_ms) / 1000u;
  const char *caller = "+1 (555) 014-2368";
  uint8_t pair_left = 0;
  switch (s_demo) {
    case Demo::Idle:
      break;
    case Demo::Discover:
      st.flags |= ELC_CAN_PHONE_PAIRING;
      pair_left = (uint8_t)(secs < 120u ? 120u - secs : 0u);
      if (!pair_left) s_demo = Demo::Idle;  // the window ran out
      break;
    case Demo::Incoming:
      st.call = ELC_CAN_CALL_INCOMING;
      st.flags |= ELC_CAN_PHONE_INBAND_RING | ELC_CAN_PHONE_CALL_AUDIO;
      break;
    case Demo::Active:
      st.call = ELC_CAN_CALL_ACTIVE;
      st.call_s = (uint16_t)secs;
      if (!s_demo_on_phone) st.flags |= ELC_CAN_PHONE_CALL_AUDIO;
      if (s_demo_muted) st.flags |= ELC_CAN_PHONE_MIC_MUTED;
      break;
    case Demo::Waiting:
      st.call = ELC_CAN_CALL_WAITING;
      st.call_s = (uint16_t)secs;
      st.flags |= ELC_CAN_PHONE_CALL_AUDIO;
      st.caller_serial = 6;
      caller = "+1 (555) 019-7731";
      break;
    case Demo::Held:
      st.call = ELC_CAN_CALL_HELD;
      st.call_s = (uint16_t)secs;
      st.flags |= ELC_CAN_PHONE_CALL_AUDIO;
      break;
    case Demo::Pair:
      st.flags = ELC_CAN_PHONE_PAIRING | ELC_CAN_PHONE_CONFIRM;
      st.connected_index = ELC_CAN_PHONE_NONE;
      st.signal = st.battery = ELC_CAN_PHONE_NONE;
      pair_left = 97;
      break;
    default: return;
  }
  if (s_demo >= Demo::Incoming && s_demo <= Demo::Held && !conn) {
    st.flags |= ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP;  // a call needs a phone
  }
  ElcCanFrame f;
  ElcCanFrame text[ELC_CAN_PHONE_TEXT_CHUNKS];
  s_demo_feeding = true;
  elc_can_encode_phone_status(0, st, f);
  ingest(f);
  if (st.call != ELC_CAN_CALL_IDLE) {
    elc_can_encode_phone_text(0, ELC_CAN_PHONE_TEXT_CALLER, st.caller_serial, caller, text);
    for (const ElcCanFrame &t : text) ingest(t);
  }
  if (st.flags & ELC_CAN_PHONE_CONNECTED) {
    const uint8_t ci = st.connected_index < st.paired ? st.connected_index : 0;
    elc_can_encode_phone_text(0, ELC_CAN_PHONE_TEXT_CONNECTED, (uint8_t)(2 + ci), kDemoPaired[ci], text);
    for (const ElcCanFrame &t : text) ingest(t);
  }
  for (uint8_t i = 0; i < st.paired; i++) {
    elc_can_encode_phone_text(0, (uint8_t)(ELC_CAN_PHONE_TEXT_PAIRED + i), st.list_serial, kDemoPaired[i], text);
    for (const ElcCanFrame &t : text) ingest(t);
  }
  if (st.flags & ELC_CAN_PHONE_CONFIRM) {
    elc_can_encode_phone_text(0, ELC_CAN_PHONE_TEXT_PAIRING, 3, "Pixel 8 Pro", text);
    for (const ElcCanFrame &t : text) ingest(t);
    elc_can_encode_phone_pairing(0, 42817, pair_left, f);  // shows as 042 817: the code keeps its leading zeros
    ingest(f);
  } else if (st.flags & ELC_CAN_PHONE_PAIRING) {
    elc_can_encode_phone_pairing(0, 0, pair_left, f);
    ingest(f);
  }
  s_demo_feeding = false;
}

// ---------------------------------------------------------------- card
// Laid out on an 800-unit circle, scaled to the panel's circle (720, 800, or the centred 600 of a 1024x600 panel).
// Fonts keep their size; positions and widths scale.
enum Tone : int8_t { kToneNone = -1, kToneNeutral = 0, kToneOk = 1, kToneBad = 2, kToneAccent = 3 };

struct Key {
  lv_obj_t *pill, *lbl, *chev;
  int8_t tone;
};

struct Card {
  lv_obj_t *root, *ring, *icon, *title, *caller, *code, *sub, *muted, *note;
  Key key[2];
  int8_t ring_tone;
  int theme;
} C = {};

UiTheme T;
float s_k = 1.0f;
bool s_showing = false;

// Chevrons drawn as lines: the fonts have no arrow glyphs.
const lv_point_t kChevLeft[3] = {{13, 0}, {0, 13}, {13, 26}};
const lv_point_t kChevRight[3] = {{0, 0}, {13, 13}, {0, 26}};

int S(int v) { return (int)lroundf((float)v * s_k); }

lv_color_t tone_color(int8_t tone) {
  switch (tone) {
    case kToneOk: return T.ok;
    case kToneBad: return T.bad;
    case kToneAccent: return T.accent;
    default: return T.line;
  }
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int w, int dy) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_style_all(l);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  if (w > 0) {
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  }
  lv_label_set_text(l, "");
  lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
  return l;
}

void set_text(lv_obj_t *l, const char *text) {
  if (strcmp(lv_label_get_text(l), text) != 0) lv_label_set_text(l, text);
}

void set_hidden(lv_obj_t *o, bool hidden) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hidden) {
    if (hidden) {
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

const lv_font_t *code_font(int theme) {
  return theme == 1 ? &elc_vt_120d : (theme == 2 ? &elc_sa_136d : &elc_bc_64d);
}

void card_destroy() {
  if (C.root) lv_obj_del(C.root);
  C = {};
  s_showing = false;
}

void card_build(int theme) {
  T = ui_theme((uint8_t)theme);
  lv_obj_t *parent = p4_screen_ui_parent();
  const int w = lv_disp_get_hor_res(nullptr), h = lv_disp_get_ver_res(nullptr);
  const int d = w < h ? w : h;
  s_k = (float)d / 800.0f;

  C.theme = theme;
  // Opaque backdrop over the whole panel, like the menu's: LVGL then skips the face underneath entirely.
  // Clickable with no handler, so touches stop here and never reach a menu that may be open below.
  C.root = lv_obj_create(parent);
  lv_obj_remove_style_all(C.root);
  lv_obj_set_size(C.root, w, h);
  lv_obj_set_pos(C.root, 0, 0);
  lv_obj_set_style_bg_color(C.root, T.bg, 0);
  lv_obj_set_style_bg_opa(C.root, LV_OPA_COVER, 0);
  lv_obj_clear_flag(C.root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(C.root, LV_OBJ_FLAG_CLICKABLE);

  C.ring = lv_obj_create(C.root);
  lv_obj_remove_style_all(C.ring);
  lv_obj_set_size(C.ring, d - S(28), d - S(28));
  lv_obj_center(C.ring);
  lv_obj_set_style_radius(C.ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(C.ring, 3, 0);
  lv_obj_set_style_border_opa(C.ring, LV_OPA_COVER, 0);
  lv_obj_clear_flag(C.ring, LV_OBJ_FLAG_CLICKABLE);
  C.ring_tone = kToneNone;

  C.icon = label(C.root, &elc_icons_56, T.accent, 0, S(-212));
  C.title = label(C.root, T.body, T.dim, S(520), S(-142));
  C.caller = label(C.root, T.label, T.text, S(600), S(-56));
  C.code = label(C.root, code_font(theme), T.text, S(600), S(-22));
  C.sub = label(C.root, T.sub, T.dim, S(540), S(28));
  C.note = label(C.root, T.sub, T.dim, S(480), S(152));

  C.muted = lv_obj_create(C.root);
  lv_obj_remove_style_all(C.muted);
  lv_obj_set_size(C.muted, S(170), S(52));
  lv_obj_align(C.muted, LV_ALIGN_CENTER, 0, S(98));
  lv_obj_set_style_radius(C.muted, S(26), 0);
  lv_obj_set_style_border_color(C.muted, T.bad, 0);
  lv_obj_set_style_border_width(C.muted, 2, 0);
  lv_obj_set_style_border_opa(C.muted, LV_OPA_COVER, 0);
  lv_obj_clear_flag(C.muted, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *ml = lv_label_create(C.muted);
  lv_obj_remove_style_all(ml);
  lv_obj_set_style_text_font(ml, T.body, 0);
  lv_obj_set_style_text_color(ml, T.bad, 0);
  lv_label_set_text(ml, "Muted");
  lv_obj_center(ml);

  for (int i = 0; i < 2; i++) {
    Key &k = C.key[i];
    const int pw = S(236), ph = S(72);
    k.pill = lv_obj_create(C.root);
    lv_obj_remove_style_all(k.pill);
    lv_obj_set_size(k.pill, pw, ph);
    lv_obj_align(k.pill, LV_ALIGN_CENTER, i == 0 ? -S(130) : S(130), S(226));
    lv_obj_set_style_radius(k.pill, ph / 2, 0);
    lv_obj_set_style_bg_color(k.pill, T.surface, 0);
    lv_obj_set_style_bg_opa(k.pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(k.pill, 2, 0);
    lv_obj_set_style_border_opa(k.pill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(k.pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(k.pill, LV_OBJ_FLAG_SCROLLABLE);
    k.lbl = lv_label_create(k.pill);
    lv_obj_remove_style_all(k.lbl);
    lv_obj_set_style_text_font(k.lbl, T.body, 0);
    lv_obj_set_style_text_color(k.lbl, T.text, 0);
    lv_label_set_text(k.lbl, "");
    lv_obj_align(k.lbl, LV_ALIGN_CENTER, i == 0 ? S(12) : -S(12), 0);
    k.chev = lv_line_create(k.pill);
    lv_line_set_points(k.chev, i == 0 ? kChevLeft : kChevRight, 3);
    lv_obj_set_style_line_width(k.chev, 4, 0);
    lv_obj_set_style_line_rounded(k.chev, true, 0);
    lv_obj_align(k.chev, i == 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, i == 0 ? S(22) : -S(22), 0);
    k.tone = kToneNone;
  }
}

// What the card shows; built each poll, applied to the labels only where it differs.
struct View {
  bool pair;
  const char *icon;
  char title[64];
  char caller[ELC_CAN_PHONE_TEXT_MAX + 1];
  char code[12];
  char sub[64];
  bool muted;
  const char *note;
  const char *key[2];  // nullptr = that key does nothing now
  int8_t tone[2];
  int8_t ring;
};

void format_duration(char *out, size_t n, uint32_t s) {
  if (s >= 3600u) {
    snprintf(out, n, "%u:%02u:%02u", (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u), (unsigned)(s % 60u));
  } else {
    snprintf(out, n, "%02u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
  }
}

/** Call time: the last status plus the time since (status comes every second, the label ticks in between). */
uint32_t call_seconds(uint32_t now) {
  uint32_t extra = (now - s_st_ms) / 1000u;
  if (extra > 2) extra = 2;
  return (uint32_t)s_st.call_s + extra;
}

void make_view(View &v, uint32_t now) {
  memset(&v, 0, sizeof(v));
  v.tone[0] = v.tone[1] = kToneNeutral;
  v.ring = kToneAccent;
  const char *name = phone_can_phone_name();
  // Same priority as the console's soft keys (docs/protocols/can_bus.md, "Console"): a code to confirm first.
  if (s_st.flags & ELC_CAN_PHONE_CONFIRM) {
    v.pair = true;
    v.icon = IC_BLUETOOTH;
    const char *pn = phone_can_pairing_name();
    if (pn[0]) {
      snprintf(v.title, sizeof(v.title), "Pair with %s?", pn);
    } else {
      snprintf(v.title, sizeof(v.title), "Pair new phone?");
    }
    uint32_t code = 0;
    if (phone_can_passkey(&code)) {
      snprintf(v.code, sizeof(v.code), "%03u %03u", (unsigned)(code / 1000u), (unsigned)(code % 1000u));
    } else {
      snprintf(v.code, sizeof(v.code), "--- ---");
    }
    snprintf(v.sub, sizeof(v.sub), "Check that the phone shows the same code");
    v.key[0] = "Reject";
    v.key[1] = "Pair";
    v.tone[0] = kToneBad;
    v.tone[1] = kToneOk;
    return;
  }
  v.icon = IC_PHONE;
  const char *caller = phone_can_caller();
  const bool have_caller = s_caller.valid() && s_caller.serial() == s_st.caller_serial;
  snprintf(v.caller, sizeof(v.caller), "%s", caller[0] ? caller : (have_caller ? "Unknown number" : ""));
  v.key[0] = "End";
  v.tone[0] = kToneBad;
  switch (s_st.call) {
    case ELC_CAN_CALL_INCOMING:
      snprintf(v.title, sizeof(v.title), "Incoming call");
      if (name[0]) snprintf(v.sub, sizeof(v.sub), "on %s", name);
      v.key[0] = "Decline";
      v.key[1] = "Answer";
      v.tone[1] = kToneOk;
      v.ring = kToneOk;
      break;
    case ELC_CAN_CALL_DIALING:
    case ELC_CAN_CALL_ALERTING:
      snprintf(v.title, sizeof(v.title), "Calling\xE2\x80\xA6");
      break;
    case ELC_CAN_CALL_WAITING:
      // One caller text: which of the two calls it names is the bridge's choice (the one that needs a decision).
      snprintf(v.title, sizeof(v.title), "Call waiting");
      snprintf(v.sub, sizeof(v.sub), "Answer puts the current call on hold");
      v.key[0] = "Ignore";
      v.key[1] = "Answer";
      v.tone[0] = kToneNeutral;
      v.tone[1] = kToneOk;
      v.ring = kToneOk;
      break;
    case ELC_CAN_CALL_HELD:
      snprintf(v.title, sizeof(v.title), "On hold");
      v.key[1] = "Swap";
      v.ring = kToneNeutral;
      break;
    case ELC_CAN_CALL_ACTIVE:
    default: {
      format_duration(v.title, sizeof(v.title), call_seconds(now));
      const bool muted = (s_st.flags & ELC_CAN_PHONE_MIC_MUTED) != 0;
      v.key[1] = muted ? "Unmute" : "Mute";
      break;
    }
  }
  if (s_st.call == ELC_CAN_CALL_ACTIVE || s_st.call == ELC_CAN_CALL_WAITING || s_st.call == ELC_CAN_CALL_HELD) {
    v.muted = (s_st.flags & ELC_CAN_PHONE_MIC_MUTED) != 0;
    if ((s_st.flags & ELC_CAN_PHONE_HFP) && !(s_st.flags & ELC_CAN_PHONE_CALL_AUDIO)) {
      v.note = "Audio on phone";
    }
  }
}

void apply_key(Key &k, const char *text, int8_t tone) {
  set_hidden(k.pill, text == nullptr);
  if (!text) return;
  set_text(k.lbl, text);
  if (k.tone != tone) {
    k.tone = tone;
    const lv_color_t c = tone_color(tone);
    lv_obj_set_style_border_color(k.pill, c, 0);
    lv_obj_set_style_line_color(k.chev, tone == kToneNeutral ? T.dim : c, 0);
  }
}

void card_apply(const View &v) {
  if (C.ring_tone != v.ring) {  // restyling the ring repaints the whole circle: only on a state change
    C.ring_tone = v.ring;
    lv_obj_set_style_border_color(C.ring, tone_color(v.ring), 0);
  }
  set_text(C.icon, v.icon);
  // The call time is the heading of an active call: in the accent colour, the state words are dimmer.
  const bool timer = !v.pair && (s_st.call == ELC_CAN_CALL_ACTIVE);
  const lv_color_t tc = timer ? T.accent : (v.pair ? T.text : T.dim);
  if (lv_obj_get_style_text_color(C.title, 0).full != tc.full) lv_obj_set_style_text_color(C.title, tc, 0);
  const lv_font_t *tf = timer ? T.title : T.body;
  if (lv_obj_get_style_text_font(C.title, 0) != tf) lv_obj_set_style_text_font(C.title, tf, 0);
  set_text(C.title, v.title);
  set_hidden(C.caller, v.pair);
  set_hidden(C.code, !v.pair);
  if (v.pair) {
    set_text(C.code, v.code);
  } else {
    set_text(C.caller, v.caller);
  }
  // The pairing hint sits under the big code; call hints sit under the caller.
  const lv_coord_t sub_y = v.pair ? S(92) : S(28);
  if (lv_obj_get_y_aligned(C.sub) != sub_y) lv_obj_align(C.sub, LV_ALIGN_CENTER, 0, sub_y);
  set_text(C.sub, v.sub);
  set_hidden(C.muted, !v.muted);
  set_hidden(C.note, v.note == nullptr);
  if (v.note) set_text(C.note, v.note);
  for (int i = 0; i < 2; i++) apply_key(C.key[i], v.key[i], v.tone[i]);
}

}  // namespace

void phone_can_begin(bool (*send)(const ElcCanFrame &f), uint8_t from_node) {
  s_send = send;
  s_node = from_node;
}

bool phone_can_frame(const ElcCanFrame &f) {
  // Bridge frames only from a range whose heartbeat says audio bridge (node numbers follow the card slot).
  s_bridges.feed(f, millis());
  if (s_bridges.reject(f, millis())) return false;
  if (s_demo != Demo::Off && !s_demo_feeding) {
    return false;  // the demo owns the state until `phone demo off`
  }
  return ingest(f);
}

void phone_can_poll(bool show_here, uint32_t now_ms) {
  if (s_demo != Demo::Off && now_ms - s_demo_sent_ms >= 250u) {
    s_demo_sent_ms = now_ms;
    demo_feed(now_ms);
  }
  // A pairing request with no call shows on this board's menu Phone page instead, when that is open here.
  const bool pair_on_page = ui_menu_is_open() && ui_menu_page() == UiMenuPage::Phone;
  const bool want = show_here && heard(now_ms) &&
                    (s_st.call != ELC_CAN_CALL_IDLE || ((s_st.flags & ELC_CAN_PHONE_CONFIRM) && !pair_on_page));
  const int theme = (int)(uint8_t)gauges_theme();
  if (C.root && C.theme != theme) {
    card_destroy();  // rebuilt below in the new theme's colours and fonts if still needed
  }
  if (!want) {
    if (C.root && s_showing) {
      lv_obj_add_flag(C.root, LV_OBJ_FLAG_HIDDEN);
      s_showing = false;
    }
    return;
  }
  if (!C.root) card_build(theme);
  View v;
  make_view(v, now_ms);
  card_apply(v);
  if (!s_showing) {
    lv_obj_clear_flag(C.root, LV_OBJ_FLAG_HIDDEN);
    s_showing = true;
  }
  // Stay on top: the menu (and the hold ring) are created on the same parent later and would cover the card.
  lv_obj_t *parent = lv_obj_get_parent(C.root);
  if (lv_obj_get_index(C.root) + 1 != lv_obj_get_child_cnt(parent)) {
    lv_obj_move_foreground(C.root);
  }
}

bool phone_can_heard() { return heard(millis()); }

const ElcCanPhoneStatus &phone_can_status() { return s_st; }

const char *phone_can_caller() {
  return (s_caller.valid() && s_caller.serial() == s_st.caller_serial) ? s_caller.text() : "";
}

const char *phone_can_phone_name() {
  return (s_name.valid() && (s_st.flags & ELC_CAN_PHONE_CONNECTED)) ? s_name.text() : "";
}

const char *phone_can_pairing_name() {
  // Sent every second while a code waits; one from before this request (or a second early) belongs to another phone.
  return (s_pair_name.valid() && (int32_t)(s_pair_name_ms - s_confirm_since_ms) > -1500) ? s_pair_name.text() : "";
}

bool phone_can_passkey(uint32_t *code) {
  if (!s_have_passkey || millis() - s_passkey_ms >= kStaleMs || !(s_st.flags & ELC_CAN_PHONE_CONFIRM)) {
    return false;
  }
  if (code) *code = s_passkey % 1000000u;
  return true;
}

bool phone_can_card_showing() { return s_showing; }

uint8_t phone_can_slot() { return s_slot; }

const char *phone_can_paired_name(uint8_t i) {
  if (i >= ELC_CAN_PHONE_MAX_PAIRED || i >= s_st.paired) return "";
  const ElcCanPhoneTextDecoder &d = s_paired[i];
  return (d.valid() && d.serial() == s_st.list_serial) ? d.text() : "";
}

bool phone_can_pairing_window(uint8_t *left) {
  if (!heard(millis()) || !(s_st.flags & ELC_CAN_PHONE_PAIRING)) return false;
  if (left) *left = (millis() - s_passkey_ms < kStaleMs) ? s_pair_left : 255;
  return true;
}

bool phone_can_send(uint8_t command, uint8_t arg) {
  if (s_demo != Demo::Off) {  // bench demo: act out what the bridge would do
    switch (command) {
      case ELC_CAN_PHONE_CMD_PAIRING:
        s_demo = arg ? Demo::Discover : Demo::Idle;
        s_demo_start_ms = millis();
        break;
      case ELC_CAN_PHONE_CMD_CONFIRM: s_demo = Demo::Idle; break;
      case ELC_CAN_PHONE_CMD_CONNECT: s_demo_conn = arg; break;
      case ELC_CAN_PHONE_CMD_DISCONNECT: s_demo_conn = ELC_CAN_PHONE_NONE; break;
      default: break;
    }
    s_demo_sent_ms = millis() - 1000u;  // fresh demo status on the next poll
    return true;
  }
  if (!s_send || !heard(millis())) return false;
  ElcCanPhoneCommandMsg c;
  c.slot = s_slot;
  c.command = command;
  c.arg = arg;
  c.from_node = s_node;
  c.list_serial = s_st.list_serial;
  ElcCanFrame f;
  elc_can_encode_phone_command(c, f);
  return s_send(f);
}

void phone_can_describe(char *out, size_t n) {
  static const char *const kCall[] = {"idle", "incoming", "dialing", "alerting", "active", "waiting", "held"};
  const uint32_t now = millis();
  if (!s_have) {
    snprintf(out, n, "phone: no status from an audio bridge yet%s", s_demo != Demo::Off ? " (demo on)" : "");
    return;
  }
  const ElcCanPhoneStatus &s = s_st;
  char t[16];
  format_duration(t, sizeof(t), s.call_s);
  uint32_t code = 0;
  const bool have_code = phone_can_passkey(&code);
  snprintf(out, n,
           "phone: %s (slot %u, status %lu ms ago)%s\n"
           "  flags 0x%02X:%s%s%s%s%s%s%s%s\n"
           "  call %s, %s, caller \"%s\" (serial %u, text serial %d)\n"
           "  phone \"%s\", signal %u, battery %u, paired %u, connected index %u, list serial %u, call volume %u\n"
           "  pairing: name \"%s\", code %06lu%s; card %s",
           heard(now) ? "bridge heard" : "bridge gone", (unsigned)s_slot, (unsigned long)(now - s_st_ms),
           s_demo != Demo::Off ? " [demo]" : "", (unsigned)s.flags,
           (s.flags & ELC_CAN_PHONE_CONNECTED) ? " connected" : "", (s.flags & ELC_CAN_PHONE_HFP) ? " hfp" : "",
           (s.flags & ELC_CAN_PHONE_CALL_AUDIO) ? " call-audio" : "", (s.flags & ELC_CAN_PHONE_MIC_MUTED) ? " muted" : "",
           (s.flags & ELC_CAN_PHONE_PAIRING) ? " pairing" : "", (s.flags & ELC_CAN_PHONE_CONFIRM) ? " confirm" : "",
           (s.flags & ELC_CAN_PHONE_INBAND_RING) ? " inband-ring" : "", (s.flags & ELC_CAN_PHONE_VOICE) ? " voice" : "",
           s.call < 7 ? kCall[s.call] : "?", t, phone_can_caller(), (unsigned)s.caller_serial,
           s_caller.valid() ? (int)s_caller.serial() : -1, phone_can_phone_name(), (unsigned)s.signal,
           (unsigned)s.battery, (unsigned)s.paired, (unsigned)s.connected_index, (unsigned)s.list_serial,
           (unsigned)s.call_volume, phone_can_pairing_name(), (unsigned long)code, have_code ? "" : " (none)",
           s_showing ? "showing" : "hidden");
  size_t len = strlen(out);
  uint8_t left = 255;
  if (phone_can_pairing_window(&left) && len < n) {
    if (left == 255) {
      len += (size_t)snprintf(out + len, n - len, "\n  pairing window open, until a phone pairs");
    } else {
      len += (size_t)snprintf(out + len, n - len, "\n  pairing window open, %u s left", (unsigned)left);
    }
  }
  for (uint8_t i = 0; i < s.paired && i < ELC_CAN_PHONE_MAX_PAIRED && len < n; i++) {
    const char *nm = phone_can_paired_name(i);
    len += (size_t)snprintf(out + len, n - len, "\n  paired %u: %s%s", (unsigned)i, nm[0] ? nm : "(name not heard yet)",
                            i == s.connected_index ? " [connected]" : "");
  }
  if (!s.paired && len < n) {
    snprintf(out + len, n - len, "\n  no paired phones");
  }
}

bool phone_can_command(const char *line, char *reply, size_t n) {
  if (strncmp(line, "phone", 5) != 0 || (line[5] != '\0' && line[5] != ' ')) {
    return false;
  }
  const char *arg = line + 5;
  while (*arg == ' ') arg++;
  if (!*arg) {
    phone_can_describe(reply, n);
    return true;
  }
  if (strncmp(arg, "demo ", 5) == 0) {
    const char *m = arg + 5;
    static const struct {
      const char *name;
      Demo mode;
    } kModes[] = {{"idle", Demo::Idle},   {"discover", Demo::Discover}, {"incoming", Demo::Incoming},
                  {"active", Demo::Active}, {"waiting", Demo::Waiting},   {"held", Demo::Held},
                  {"pair", Demo::Pair},     {"off", Demo::Off}};
    for (const auto &e : kModes) {
      const size_t len = strlen(e.name);
      if (strncmp(m, e.name, len) != 0 || (m[len] != '\0' && m[len] != ' ')) continue;
      const bool was_on = s_demo != Demo::Off;
      s_demo = e.mode;
      s_demo_muted = strstr(m + len, "muted") != nullptr;
      s_demo_on_phone = strstr(m + len, "onphone") != nullptr;
      s_demo_start_ms = millis();
      s_demo_sent_ms = s_demo_start_ms - 1000u;  // feed on the next poll
      s_demo_conn = 0;
      if (e.mode == Demo::Off) {
        if (was_on) s_have = false;  // drop the synthetic state; the real bridge's next status brings it back
        snprintf(reply, n, "ok: phone demo off");
      } else {
        snprintf(reply, n, "ok: phone demo %s", m);
      }
      return true;
    }
  }
  snprintf(reply, n, "usage: phone | phone demo idle|discover|incoming|active [muted] [onphone]|waiting|held|pair|off");
  return true;
}
