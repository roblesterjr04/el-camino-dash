// The console's screens: the El Camino Cluster prototype's OLED drawing (positions, grey levels 0..15, the theme's
// fonts) for the drive pages and warnings, plus the head unit's own: the radio, the menu, calls, pairing, and the
// soft-key labels (drawn in the style of the warnings).

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <accessory_can.h>

#include "console_ui.h"
#include "oled_gfx.h"

LV_FONT_DECLARE(con_cl_huge);
LV_FONT_DECLARE(con_cl_big);
LV_FONT_DECLARE(con_cl_mid);
LV_FONT_DECLARE(con_cl_body);
LV_FONT_DECLARE(con_cl_small);
LV_FONT_DECLARE(con_mo_huge);
LV_FONT_DECLARE(con_mo_big);
LV_FONT_DECLARE(con_mo_mid);
LV_FONT_DECLARE(con_mo_body);
LV_FONT_DECLARE(con_mo_small);
LV_FONT_DECLARE(con_fo_huge);
LV_FONT_DECLARE(con_fo_big);
LV_FONT_DECLARE(con_fo_mid);
LV_FONT_DECLARE(con_fo_body);
LV_FONT_DECLARE(con_fo_small);
LV_FONT_DECLARE(con_tiny);

#ifndef CONSOLE_KEY_LEFT_X
#define CONSOLE_KEY_LEFT_X 45  // centre of the left key's label, px (the Back key under the OLED)
#endif
#ifndef CONSOLE_KEY_RIGHT_X
#define CONSOLE_KEY_RIGHT_X 211
#endif

namespace {

const lv_font_t *const TINY = &con_tiny;

bool blink(uint32_t t, uint32_t period) { return (t / period) % 2 == 0; }

void mmss(char *out, size_t n, int s) {
  if (s < 0) s = 0;
  if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

/** Civil date from days since 1970-01-01 (Howard Hinnant's algorithm). */
void civil(int32_t z, int *y, int *m, int *d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = (uint32_t)(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*m <= 2));
}

}  // namespace

const lv_font_t *ConsoleUi::font(char role) const {
  const uint8_t th = _cl.theme;  // 0 Classic, 1 Fallout, 2 Modern
  if (th == 1) {
    switch (role) { case 'h': return &con_fo_huge; case 'b': return &con_fo_big; case 'm': return &con_fo_mid; case 'o': return &con_fo_body; default: return &con_fo_small; }
  }
  if (th == 2) {
    switch (role) { case 'h': return &con_mo_huge; case 'b': return &con_mo_big; case 'm': return &con_mo_mid; case 'o': return &con_mo_body; default: return &con_mo_small; }
  }
  switch (role) { case 'h': return &con_cl_huge; case 'b': return &con_cl_big; case 'm': return &con_cl_mid; case 'o': return &con_cl_body; default: return &con_cl_small; }
}

// ---- drive pages ----

void ConsoleUi::draw_media(OledGfx &g, uint32_t t) {
  const bool bridge = _tr_ms && t - _tr_ms < 3000;
  const bool media = bridge && _tr.connected;
  const int tx = 62;
  if (media && _art_id && _art_id == _tr.art_id) {
    g.gray48(4, 8, _art);
    g.rrect(4, 8, 48, 48, 4, 5);
  } else {
    g.rrect(4, 8, 48, 48, 4, 4);
    g.icon("note", 22, 26, media ? 9 : 4);
  }
  if (!media) {
    g.text("No phone", tx, 28, font('m'), 14);
    g.text("Hold the left knob: Menu \xC2\xB7 Phone", tx, 46, font('s'), 7, TextAlign::Left, 180);
    return;
  }
  // "Artist - Title" from the bridge
  char artist[40] = {0}, title[40] = {0};
  const char *np = _np.valid() ? _np.text() : "";
  const char *sep = strstr(np, " - ");
  if (sep) {
    snprintf(artist, sizeof(artist), "%.*s", (int)(sep - np), np);
    snprintf(title, sizeof(title), "%s", sep + 3);
  } else {
    snprintf(title, sizeof(title), "%s", np[0] ? np : "Not playing");
  }
  const lv_font_t *mid = font('m');
  const int tw = OledGfx::text_width(title, mid), room = 172;
  g.clip(tx, 0, room, 32);
  if (tw > room) {  // marquee
    const int span = tw + 30;
    const int x = tx - (int)((t / 40) % (uint32_t)span);
    g.text(title, x, 25, mid, 15);
    g.text(title, x + span, 25, mid, 15);
  } else {
    g.text(title, tx, 25, mid, 15);
  }
  g.no_clip();
  g.text(artist, tx, 39, font('o'), 8, TextAlign::Left, 176);
  const float frac = _tr.duration_s ? (float)_tr.elapsed_s / _tr.duration_s : 0.0f;
  g.bar(tx, 46, 176, 3, frac, 12, 2);
  char a[16], b[16];
  mmss(a, sizeof(a), _tr.elapsed_s);
  b[0] = '-';
  mmss(b + 1, sizeof(b) - 1, (int)_tr.duration_s - (int)_tr.elapsed_s);
  g.text(a, tx, 60, TINY, 7);
  g.text(b, tx + 176, 60, TINY, 7, TextAlign::Right);
  draw_phone_status(g, t);
}

/** Top right of the media page: the phone's signal (five bars) and battery, as the phone reports them over
 *  hands-free (0..5 each; a value it does not send is left out). Above the title, which starts at y 12. */
void ConsoleUi::draw_phone_status(OledGfx &g, uint32_t t) {
  if (!phone_fresh(t) || !(_ph.flags & ELC_CAN_PHONE_CONNECTED)) return;
  int x = 254;  // right edge
  if (_ph.battery <= 5) {
    const int w = 13, h = 7, y = 2;
    x -= w + 2;
    g.rect(x, y, w, h, 9);
    g.box(x + w, y + 2, 2, 3, 9);  // the nub
    const int fill = (w - 4) * _ph.battery / 5;
    const bool low = _ph.battery <= 1;
    if (fill > 0) g.box(x + 2, y + 2, fill, h - 4, low && (t / 500) % 2 ? 4 : (low ? 15 : 12));
    x -= 6;
  }
  if (_ph.signal <= 5) {
    for (int i = 4; i >= 0; i--) {  // bars 2 px wide, 1 px apart, 2..10 px tall, bottoms on y 9
      const int bh = 2 + i * 2;
      x -= i == 4 ? 2 : 3;
      g.box(x, 10 - bh, 2, bh, i < _ph.signal ? 12 : 3);
    }
  }
}

void ConsoleUi::draw_trip(OledGfx &g) {
  const bool fresh = _sensor_ms && _now - _sensor_ms < 1000;
  char c[3][3][24];
  snprintf(c[0][0], 24, "TRIP A");
  if (_s.odo_valid) snprintf(c[0][1], 24, "%.1f", trip_mi());  // 0.0 until the first reset, as on the prototype
  else snprintf(c[0][1], 24, "--.-");
  snprintf(c[0][2], 24, "MILES");
  snprintf(c[1][0], 24, "AVG MPG");
  if (fresh && _s.econ) snprintf(c[1][1], 24, "%.1f", _s.avg);
  else snprintf(c[1][1], 24, "--.-");
  if (fresh && _s.econ) snprintf(c[1][2], 24, "NOW %d", (int)(_s.inst + 0.5f));
  else snprintf(c[1][2], 24, "NOW --");
  snprintf(c[2][0], 24, "RANGE");
  if (fresh && _s.econ) snprintf(c[2][1], 24, "%d", (int)(_s.dte + 0.5f));
  else snprintf(c[2][1], 24, "---");
  if (fresh) snprintf(c[2][2], 24, "MI \xC2\xB7 %.1f GAL", _s.fuel / 100 * _s.tank);
  else snprintf(c[2][2], 24, "MI \xC2\xB7 -- GAL");
  for (int i = 0; i < 3; i++) {
    const int x = 6 + i * 80;
    g.text(c[i][0], x, 10, TINY, 7);
    g.text(c[i][1], x - 1, 44, font('b'), 15, TextAlign::Left, 76);
    g.text(c[i][2], x, 58, TINY, 6, TextAlign::Left, 76);
    if (i) g.box(x - 6, 4, 1, 56, 2);
  }
}

void ConsoleUi::draw_engine(OledGfx &g, uint32_t t) {
  const bool f = _sensor_ms && t - _sensor_ms < 1000;
  char v[4][16];
  if (f) {
    snprintf(v[0], 16, "%d psi", (int)(_s.oil + 0.5f));
    snprintf(v[1], 16, "%d\xC2\xB0" "F", (int)(_s.coolant + 0.5f));
    snprintf(v[2], 16, "%d%%", (int)_s.fuel);
    snprintf(v[3], 16, "%s", _s.charging == 1 ? "CHG" : (_s.charging == 0 ? "NO CHG" : "KEY OFF"));
  } else {
    for (auto &s : v) snprintf(s, 16, "--");
  }
  const char *label[4] = {"OIL", "COOLANT", "FUEL", "CHARGE"};
  const float frac[4] = {f ? _s.oil / 60 : 0, f ? (_s.coolant - 100) / 160 : 0, f ? _s.fuel / 100 : 0, f ? (_s.amp + 100) / 200 : 0};
  const bool warn[4] = {f && _s.rpm > 400 && _s.oil < 8, f && _s.coolant > 230, f && _s.fuel < 12, f && _s.charging == 0};
  const bool bl = blink(t, 400);
  for (int i = 0; i < 4; i++) {
    const int x = 4 + (i % 2) * 124, y = i < 2 ? 2 : 34;
    g.text(label[i], x, y + 9, TINY, warn[i] ? 15 : 7);
    if (warn[i] && bl) g.icon("warn", x + (int)strlen(label[i]) * 6 + 4, y, 15);
    g.text(v[i], x + 114, y + 18, font('m'), warn[i] ? 15 : 13, TextAlign::Right);
    g.bar(x, y + 23, 114, 4, frac[i], warn[i] ? 15 : 10, 2);
  }
}

void ConsoleUi::draw_clock(OledGfx &g, uint32_t t) {
  const bool valid = _time_ms && (_time.flags & ELC_CAN_TIME_VALID) && t - _time_ms < 10000;
  int hour, minute;
  char date[32] = {0};
  if (valid) {
    const int64_t local = (int64_t)_time.utc + (int64_t)_time.offset_min * 60 + (t - _time_ms) / 1000;
    const int32_t days = (int32_t)(local / 86400);
    const int32_t sod = (int32_t)(local % 86400);
    hour = sod / 3600;
    minute = (sod / 60) % 60;
    int y, m, d;
    civil(days, &y, &m, &d);
    static const char *const kDow[7] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};  // 1970-01-01 was a Thursday
    static const char *const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(date, sizeof(date), "%s, %s %d", kDow[((days % 7) + 7) % 7], kMon[(m - 1) % 12], d);
  } else {  // no time source: the clock counts from power-up
    const uint32_t s = (t - _boot_ms) / 1000;
    hour = (int)((s / 3600) % 24);
    minute = (int)((s / 60) % 60);
    snprintf(date, sizeof(date), "No time source");
  }
  char ts[8];
  snprintf(ts, sizeof(ts), "%d:%02d", (hour + 11) % 12 + 1, minute);
  const int w = g.text(ts, 6, 50, font('h'), 15);
  g.text(hour < 12 ? "AM" : "PM", 10 + w, 50, TINY, 8);
  const int x = 150;
  g.text(date, x, 20, font('o'), 12, TextAlign::Left, 96);
  const bool gps = valid && (_time.flags & ELC_CAN_TIME_GPS);
  g.text(valid ? "GPS time" : "Boot clock", x, 37, font('o'), 8, TextAlign::Left, 96);  // the center sets it from GPS only
  g.text(gps ? "GPS FIX" : (valid ? "NO FIX" : "BOOT CLOCK"), x, 55, TINY, 6);
  if (gps) g.icon("gps", x + 46, 46, 6);
}

/** The media page while the radio is the source: frequency and station, radio text, preset, stereo, signal. */
void ConsoleUi::draw_radio(OledGfx &g, uint32_t t) {
  const bool have = _now_ms && t - _now_ms < 5000 && _now_playing.source == ELC_SRC_RADIO;
  const ElcAudioNow &np = _now_playing;
  g.rrect(4, 8, 48, 48, 4, 5);
  const bool am = have && strstr(np.line1, " AM") != nullptr;
  g.text(am ? "AM" : "FM", 28, 30, font('m'), have ? 12 : 5, TextAlign::Center);
  g.text("RADIO", 28, 48, TINY, 6, TextAlign::Center);
  const int tx = 62;
  char top[24] = "RADIO";
  if (have && np.preset) snprintf(top, sizeof(top), "PRESET %u", (unsigned)np.preset);
  g.text(top, tx, 9, TINY, 7);
  if (have && (np.flags & ELC_NOW_F_STEREO)) g.text("ST", 200, 9, TINY, 10);
  if (have && (np.flags & ELC_NOW_F_MUTED)) g.text("MUTED", 160, 9, TINY, blink(t, 600) ? 15 : 6);
  if (have && np.signal <= 5) {  // five bars, as for the phone
    int x = 254;
    for (int i = 4; i >= 0; i--) {
      const int bh = 2 + i * 2;
      x -= i == 4 ? 2 : 3;
      g.box(x, 10 - bh, 2, bh, i < np.signal ? 12 : 3);
    }
  }
  const char *l1 = have ? np.line1 : "Radio";
  const lv_font_t *mid = font('m');
  const int tw = OledGfx::text_width(l1, mid), room = 186;
  g.clip(tx, 10, room, 22);
  if (tw > room) {  // marquee
    const int span = tw + 30;
    const int x = tx - (int)((t / 40) % (uint32_t)span);
    g.text(l1, x, 29, mid, 15);
    g.text(l1, x + span, 29, mid, 15);
  } else {
    g.text(l1, tx, 29, mid, (have && (np.flags & ELC_NOW_F_SEEKING)) && blink(t, 300) ? 8 : 15);
  }
  g.no_clip();
  const char *l2 = have && np.line2[0] ? np.line2 : (have ? "" : "Waiting for the radio card");
  g.text(l2, tx, 45, font('o'), 8, TextAlign::Left, 186);
  g.text("TURN TUNE \xC2\xB7 PUSH PRESET \xC2\xB7 HOLD SEEK", tx, 60, TINY, 5, TextAlign::Left, 186);
}

void ConsoleUi::draw_page(OledGfx &g, int page, uint32_t t) {
  switch (page) {
    case kPageMedia:
      if (radio_is_source(t)) draw_radio(g, t);
      else draw_media(g, t);
      break;
    case kPageTrip: draw_trip(g); break;
    case kPageEngine: draw_engine(g, t); break;
    default: draw_clock(g, t); break;
  }
}

// ---- OLED menu, the round-screen menu, warnings, calls ----

void ConsoleUi::draw_menu(OledGfx &g, uint32_t t) {
  static const char *const kTitle[11] = {"MENU", "SOURCE", "PHONE", "PHONE", "DISPLAY", "TRIP", "SERVICE",
                                         "ACCESSORIES", "", "", "CONFIRM"};
  build_rows();
  Level &L = _stack[_depth];
  const int rowH = 17, y0 = 12;
  if (L.sel < L.top) L.top = L.sel;
  if (L.sel > L.top + 2) L.top = L.sel - 2;
  const bool edit = _mode == ConsoleMode::Edit;
  char title[48];
  if (L.page == kMAccPage || L.page == kMAccChoice) {
    const AccItem *g2 = L.page == kMAccChoice ? accessory_can_item(L.node, L.arg)
                                              : (L.arg != ELC_UI_TOP ? accessory_can_item(L.node, L.arg) : nullptr);
    snprintf(title, sizeof(title), "%s%s%s", accessory_can_name(L.node), g2 ? " \xC2\xB7 " : "", g2 ? g2->d.label : "");
    for (char *c = title; *c; c++) *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
  } else if (L.page == kMPhoneOne) {
    const char *pn = phone_paired_name(L.arg);
    snprintf(title, sizeof(title), "%s", pn[0] ? pn : "PHONE");
  } else {
    snprintf(title, sizeof(title), "%s", kTitle[L.page % 11]);
  }
  g.text(edit ? "TURN \xC2\xB7 PUSH OK \xC2\xB7 LEFT KEY UNDO" : title, 4, 8, TINY, edit ? 10 : 6,
         TextAlign::Left, 210);
  char n[8];
  snprintf(n, sizeof(n), "%d/%d", _nrows ? L.sel + 1 : 0, _nrows);
  g.text(n, 250, 8, TINY, 5, TextAlign::Right);
  const lv_font_t *body = font('o');
  for (int r = 0; r < 3; r++) {
    const int i = L.top + r;
    if (i >= _nrows) break;
    const ConsoleRow &row = _rows[i];
    const int y = y0 + r * rowH;
    const bool sel = i == L.sel;
    const bool editing = sel && edit;
    const bool info = row.kind == ConsoleRow::Info;
    if (sel && !editing) g.rbox(1, y, 246, rowH - 1, 3, info ? 6 : 13);
    if (editing) g.rrect(1, y, 246, rowH - 1, 3, 15);
    const uint8_t fg = sel && !editing ? 0 : (row.dim ? 5 : (info ? 8 : 11));
    g.text(row.label, 7, y + 12, body, fg, TextAlign::Left, 130);
    int vx = 238;
    if (row.kind == ConsoleRow::Sub) {
      g.icon("ext", 232, y + 2, sel && !editing ? 0 : 7);
      vx = 228;
    }
    if (editing) {
      const bool bl = blink(t, 350);
      const int vw = OledGfx::text_width(row.value, body);
      g.text("\xE2\x80\xB9", 238 - vw - 10, y + 12, body, bl ? 15 : 6);
      g.text("\xE2\x80\xBA", 240, y + 12, body, bl ? 15 : 6);
      g.text(row.value, 236, y + 12, body, 15, TextAlign::Right);
    } else {
      g.text(row.value, vx, y + 12, body, sel ? 0 : (row.dim ? 4 : 8), TextAlign::Right, 110);
    }
  }
  if (_nrows > 3) {  // scrollbar
    g.box(251, 12, 2, 50, 2);
    const float th = fmaxf(8, 50.0f * 3 / _nrows);
    const float ty = 12 + (50 - th) * ((float)L.top / (_nrows - 3));
    g.box(251, (int)lroundf(ty), 2, (int)lroundf(th), 9);
  }
}

void ConsoleUi::draw_alert(OledGfx &g, const ConsoleAlert &a, uint32_t t) {
  const bool bl = blink(t, 450);
  g.rect(0, 0, 256, 64, bl ? 15 : 6);
  g.rect(2, 2, 252, 60, bl ? 6 : 2);
  // the warning triangle at 2.4×
  g.tri(10 + 6 * 2.4f, 16, 10 + 12 * 2.4f, 16 + 11 * 2.4f, 10, 16 + 11 * 2.4f, 15);
  g.box(10 + (int)(5.4f * 2.4f), 16 + (int)(3.5f * 2.4f), 3, 10, 0);
  g.box(10 + (int)(5.4f * 2.4f), 16 + (int)(8.8f * 2.4f), 3, 3, 0);
  g.text(a.title, 50, 30, font('m'), 15, TextAlign::Left, 200);
  g.text(a.line, 50, 45, font('s'), 10, TextAlign::Left, 200);
  g.text("LEFT KEY TO ACKNOWLEDGE", 50, 58, TINY, 5, TextAlign::Left, 200);
}

/** A soft-key label over one of the keys under the OLED: an inverted pill centred on the key. */
static void soft_key(OledGfx &g, const char *label, int cx) {
  if (!label || !label[0]) return;
  const int w = OledGfx::text_width(label, &con_tiny) + 12;
  g.rbox(cx - w / 2, 49, w, 13, 3, 13);
  g.text(label, cx, 59, &con_tiny, 0, TextAlign::Center);
}

void ConsoleUi::draw_call(OledGfx &g, uint32_t t) {
  const uint8_t c = _ph.call;
  const bool ringing = c == ELC_CAN_CALL_INCOMING || c == ELC_CAN_CALL_WAITING;
  const bool bl = blink(t, 450);
  if (ringing) {
    g.rect(0, 0, 256, 64, bl ? 15 : 6);
    g.rect(2, 2, 252, 60, bl ? 6 : 2);
  }
  char title[32], sub[40] = {0};
  const char *left = "End", *right = "";
  switch (c) {
    case ELC_CAN_CALL_INCOMING: snprintf(title, sizeof(title), "INCOMING CALL"); left = "Decline"; right = "Answer"; break;
    case ELC_CAN_CALL_WAITING: snprintf(title, sizeof(title), "CALL WAITING"); left = "Ignore"; right = "Answer"; break;
    case ELC_CAN_CALL_DIALING: snprintf(title, sizeof(title), "CALLING"); break;
    case ELC_CAN_CALL_ALERTING: snprintf(title, sizeof(title), "CALLING \xC2\xB7 RINGING"); break;
    case ELC_CAN_CALL_HELD: snprintf(title, sizeof(title), "ON HOLD"); right = "Swap"; break;
    default: {
      char d[16];
      mmss(d, sizeof(d), _ph.call_s);
      snprintf(title, sizeof(title), "IN CALL \xC2\xB7 %s", d);
      right = (_ph.flags & ELC_CAN_PHONE_MIC_MUTED) ? "Unmute" : "Mute";
      break;
    }
  }
  if (c == ELC_CAN_CALL_ACTIVE || c == ELC_CAN_CALL_HELD) {
    if (_ph.flags & ELC_CAN_PHONE_MIC_MUTED) snprintf(sub, sizeof(sub), "MUTED");
    else if (!(_ph.flags & ELC_CAN_PHONE_CALL_AUDIO)) snprintf(sub, sizeof(sub), "AUDIO ON PHONE");
  }
  const bool have = _caller.valid() && _caller.serial() == _ph.caller_serial;
  const char *who = have ? (_caller.text()[0] ? _caller.text() : "Unknown number") : "\xE2\x80\xA6";
  g.icon("phone", 10, 8, 15);
  g.text(title, 28, 17, TINY, ringing ? 15 : 9);
  if (sub[0]) g.text(sub, 250, 17, TINY, 15, TextAlign::Right);
  g.text(who, 10, 39, font('m'), 15, TextAlign::Left, 236);
  soft_key(g, left, CONSOLE_KEY_LEFT_X);
  soft_key(g, right, CONSOLE_KEY_RIGHT_X);
}

/** A phone asks to pair: its name and the code to compare, Reject / Pair on the soft keys. */
void ConsoleUi::draw_pair(OledGfx &g, uint32_t t) {
  const bool bl = blink(t, 450);
  g.rect(0, 0, 256, 64, bl ? 15 : 6);
  g.rect(2, 2, 252, 60, bl ? 6 : 2);
  g.icon("phone", 10, 8, 15);
  const char *who = _ph_pairing.valid() && _ph_pairing.text()[0] ? _ph_pairing.text() : "A phone";
  char title[48];
  snprintf(title, sizeof(title), "PAIR WITH %s?", who);
  for (char *c = title; *c; c++) *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
  g.text(title, 28, 17, TINY, 15, TextAlign::Left, 220);
  char code[16];
  if (_have_passkey) snprintf(code, sizeof(code), "%03u %03u", (unsigned)(_passkey / 1000u), (unsigned)(_passkey % 1000u));
  else snprintf(code, sizeof(code), "--- ---");
  g.text(code, 128, 42, font('b'), 15, TextAlign::Center);
  soft_key(g, "Reject", CONSOLE_KEY_LEFT_X);
  soft_key(g, "Pair", CONSOLE_KEY_RIGHT_X);
}

/** At rest, after a control was touched: what the soft keys do now, over the keys. */
void ConsoleUi::draw_hints(OledGfx &g, uint32_t t) {
  if (!_touch_ms || t - _touch_ms > kHintMs || _mode != ConsoleMode::Drive) return;
  const bool radio_src = radio_is_source(t);
  const char *left = radio_src ? ((_now_ms && (_now_playing.flags & ELC_NOW_F_MUTED)) ? "Unmute" : "Mute") : "Play/Pause";
  static const char *const kNext[kPageCount] = {"Trip", "Engine", "Clock", "Media"};
  g.box(0, 47, 248, 17, 0);  // over the page's bottom line
  soft_key(g, left, CONSOLE_KEY_LEFT_X);
  soft_key(g, kNext[_page % kPageCount], CONSOLE_KEY_RIGHT_X);
}

void ConsoleUi::draw_volume(OledGfx &g, uint32_t t) {
  if (!_vol_ms) return;
  const uint32_t age = t - _vol_ms;
  const float a = fmaxf(0, fminf(1, 1 - ((float)age - 1200) / 250));
  if (a <= 0 || age > kVolumeShowMs) return;
  const int y = (int)lroundf(64 - 26 * fminf(1, age / 120.0f) * a);
  const bool call = call_shown(t);
  const int v = call ? _ph.call_volume : _vol;
  g.box(0, y - 1, 256, 28, 0);
  g.box(0, y - 1, 256, 1, 4);
  g.icon("spk", 6, y + 6, 13);
  g.text(call ? "CALL VOLUME" : "VOLUME", 22, y + 11, TINY, 7);
  for (int i = 0; i < 15; i++) g.box(22 + (int)(i * 13.6f), y + 15, 12, 7, i < v ? 14 : 2);  // the prototype's bar, 15 steps
  char n[4];
  snprintf(n, sizeof(n), "%d", v);
  g.text(n, 250, y + 11, TINY, 12, TextAlign::Right);
}

void ConsoleUi::draw_toast(OledGfx &g, uint32_t t) {
  if (!_toast_ms) return;
  const uint32_t age = t - _toast_ms;
  if (age > kToastMs) {
    _toast_ms = 0;
    return;
  }
  const int h = 15;
  const float k = fminf(1, fminf(age / 140.0f, (kToastMs - age) / 140.0f));
  const int y = (int)lroundf(-h + h * k);
  g.box(0, y, 256, h, 12);
  g.text(_toast_title, 5, y + 11, TINY, 0);
  g.text(_toast_value, 250, y + 12, font('o'), 0, TextAlign::Right);
}

// ---- the frame ----

void ConsoleUi::render(OledGfx &g, uint32_t t) {
  _now = t;
  g.no_clip();
  g.offset(0, 0);
  g.clear();
  if (_boot_ms == 0) _boot_ms = t ? t : 1;
  const uint32_t up = t - _boot_ms;
  // A highlight that sweeps left to right across the bowtie in `dur` ms, from `start`.
  const auto sweep = [&](uint32_t since, uint32_t dur) {
    if (since >= dur) return;
    const float f = (float)since / dur;
    g.sheen(40 + f * 200, 12, 1.1f, 5.0f);
  };
  if (up < kBootMs) {  // fade in, then one sweep
    g.bowtie((int)fminf(8, up / 90.0f), up > 400 ? 2 : (up > 200 ? 1 : 0));
    if (up > 800) sweep(up - 800, kBootMs - 800);
    return;
  }
  if (!_booted || t - _center_ms >= kCenterGoneMs) {
    g.bowtie(8, 2);
    sweep((t - _boot_ms) % 3500, 900);  // again every 3.5 s while waiting
    g.text("WAITING FOR CAN", 128, 60, TINY, 4, TextAlign::Center);
    return;
  }
  const bool reverse = _cs_ms && (_cs.flags & ELC_CAN_STATUS_REVERSE);
  if (call_shown(t)) {
    draw_call(g, t);
  } else if (pair_shown(t)) {
    draw_pair(g, t);
  } else if (reverse) {
    g.text("R", 10, 50, font('h'), 15);
    g.text("REVERSE", 50, 24, font('m'), 15);
    g.text("Rear camera on the center display", 50, 42, font('s'), 9, TextAlign::Left, 200);
    char mph[16];
    const bool f = _sensor_ms && t - _sensor_ms < 1000;
    if (f) snprintf(mph, sizeof(mph), "%d MPH", (int)(_s.mph + 0.5f));
    else snprintf(mph, sizeof(mph), "-- MPH");
    g.text(mph, 50, 58, TINY, 6);
  } else {
    const ConsoleAlert *al = alert(t);
    if (al && _mode == ConsoleMode::Drive) {
      draw_alert(g, *al, t);
    } else if (_mode == ConsoleMode::Menu || _mode == ConsoleMode::Edit) {
      draw_menu(g, t);
    } else {
      const float e = fminf(1, (t - _page_ms) / 180.0f);
      const float ez = 1 - powf(1 - e, 3);
      if (_page_ms && e < 1) {
        g.offset(0, (int)lroundf(-_page_dir * 64 * ez));
        draw_page(g, _page_from, t);
        g.offset(0, (int)lroundf(_page_dir * 64 * (1 - ez)));
        draw_page(g, _page, t);
        g.offset(0, 0);
      } else {
        draw_page(g, _page, t);
      }
      for (int i = 0; i < kPageCount; i++) {  // page dots
        const bool on = i == _page;
        g.box(252, 17 + i * 8 + (on ? 0 : 1), 3, on ? 6 : 3, on ? 13 : 3);
      }
      if (_n_alerts && blink(t, 500)) g.icon("warn", 238, 50, 15);
      draw_hints(g, t);
    }
  }
  draw_toast(g, t);
  draw_volume(g, t);
}
