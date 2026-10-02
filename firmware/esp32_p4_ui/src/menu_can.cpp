#include "menu_can.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <phone_can.h>
#include <ui_menu.h>

#if defined(ARDUINO)
#include <Arduino.h>
#else
extern "C" uint32_t millis(void);
#endif

namespace {

MenuCanConfig C;

// Console input, queued from the CAN handler and applied in menu_can_poll().
ElcCanMenuInput s_q[8];
uint8_t s_qh = 0, s_qn = 0;

bool s_was_open = false;
uint8_t s_opened_by = ELC_CAN_MENU_BY_TOUCH;
bool s_confirm_was = false;

// What went out last (texts with it), and when.
ElcCanMenuState s_sent;
char s_sent_main[ELC_CAN_TEXT_MAX + 1] = "", s_sent_sub[ELC_CAN_TEXT_MAX + 1] = "";
bool s_sent_valid = false;
uint8_t s_serial = 0;
uint32_t s_sent_ms = 0, s_check_ms = 0;
bool s_force = false;  // console input just arrived: answer with the outcome now

bool interface_here() { return C.is_interface && C.is_interface(); }

void apply(const ElcCanMenuInput &in) {
  if (!interface_here()) {
    return;  // only the interface screen follows the console
  }
  const bool was_open = ui_menu_is_open();
  if (C.before_open && ((in.event == ELC_CAN_MENU_PRESS && !was_open) || in.event == ELC_CAN_MENU_OPEN)) {
    C.before_open();
  }
  switch (in.event) {
    case ELC_CAN_MENU_ROTATE:
      if (was_open) ui_menu_rotate(in.value);
      break;
    case ELC_CAN_MENU_PRESS: ui_menu_press(); break;  // opens the menu when closed
    case ELC_CAN_MENU_BACK: ui_menu_back(); break;
    case ELC_CAN_MENU_OPEN:
      if ((uint8_t)in.value < kUiMenuPages) ui_menu_open((UiMenuPage)(uint8_t)in.value);
      break;
    default: return;
  }
  if (!was_open && ui_menu_is_open()) {
    s_opened_by = ELC_CAN_MENU_BY_CONSOLE;
    s_was_open = true;
  }
  s_force = true;
}

void queue(const ElcCanMenuInput &in) {
  if (s_qn < 8) {
    s_q[(s_qh + s_qn) % 8] = in;
    s_qn++;
  }
}

/** The state this board would send now, with its texts. */
void current(ElcCanMenuState &st, char *m, size_t nm, char *sb, size_t ns) {
  st.open = ui_menu_is_open();
  st.screen = C.self_screen;
  st.page = st.open ? (uint8_t)ui_menu_page() : (uint8_t)ELC_CAN_PAGE_HOME;
  st.opened_by = s_opened_by;
  ui_menu_describe(m, nm, sb, ns);
}

void send(const ElcCanMenuState &st, const char *m, const char *sb) {
  ElcCanFrame f;
  elc_can_encode_menu_state(st, f);
  C.send(f);
  ElcCanFrame t[ELC_CAN_TEXT_CHUNKS];
  elc_can_encode_text(ELC_CAN_ID_MENU_TEXT_MAIN, st.text_serial, m, t);
  for (const ElcCanFrame &c : t) C.send(c);
  elc_can_encode_text(ELC_CAN_ID_MENU_TEXT_SUB, st.text_serial, sb, t);
  for (const ElcCanFrame &c : t) C.send(c);
}

}  // namespace

void menu_can_begin(const MenuCanConfig &cfg) { C = cfg; }

bool menu_can_frame(const ElcCanFrame &f) {
  ElcCanMenuInput in;
  if (!elc_can_decode_menu_input(f, in)) {
    return false;
  }
  queue(in);
  return true;
}

void menu_can_poll(uint32_t now_ms) {
  while (s_qn) {
    const ElcCanMenuInput in = s_q[s_qh];
    s_qh = (uint8_t)((s_qh + 1) % 8);
    s_qn--;
    apply(in);
  }
  const bool iface = interface_here();

  // A phone asks to pair: its code is confirmed on the Phone page, so bring that up on the interface screen.
  const bool confirm = phone_can_heard() && (phone_can_status().flags & ELC_CAN_PHONE_CONFIRM);
  if (confirm && !s_confirm_was && iface && (!C.may_open || C.may_open())) {
    if (!ui_menu_is_open() || ui_menu_page() != UiMenuPage::Phone) {
      if (!ui_menu_is_open()) {
        s_opened_by = ELC_CAN_MENU_BY_TOUCH;  // not the console's doing
        s_was_open = true;
      }
      if (C.before_open) C.before_open();
      ui_menu_open(UiMenuPage::Phone);
      s_force = true;
    }
  }
  s_confirm_was = confirm;

  const bool open = ui_menu_is_open();
  if (open && !s_was_open) {
    s_opened_by = ELC_CAN_MENU_BY_TOUCH;  // opened here without the console (a tap, the bench knob)
  }
  if (open != s_was_open) s_force = true;
  s_was_open = open;

  if (!C.send) {
    return;
  }
  // Only the interface screen talks, plus one closing state from a screen whose menu was open when it stopped
  // being the interface screen.
  if (!iface && !(s_sent_valid && s_sent.open)) {
    s_force = false;
    return;
  }
  const bool force = s_force && now_ms - s_sent_ms >= 40u;  // a fast-turning knob: at most 25 states a second
  if (!force && now_ms - s_check_ms < 100u) {
    return;
  }
  s_check_ms = now_ms;
  ElcCanMenuState st;
  char m[64], sb[64];
  current(st, m, sizeof(m), sb, sizeof(sb));
  const bool texts_changed = !s_sent_valid || strcmp(m, s_sent_main) != 0 || strcmp(sb, s_sent_sub) != 0;
  if (texts_changed) {
    s_serial = (uint8_t)((s_serial + 1) & 0x0F);
  }
  st.text_serial = s_serial;
  const bool changed = texts_changed || st.open != s_sent.open || st.page != s_sent.page ||
                       st.opened_by != s_sent.opened_by || st.screen != s_sent.screen;
  if (!changed && !(open && now_ms - s_sent_ms >= 1000u)) {
    s_force = false;
    return;
  }
  send(st, m, sb);
  s_sent = st;
  snprintf(s_sent_main, sizeof(s_sent_main), "%s", m);
  snprintf(s_sent_sub, sizeof(s_sent_sub), "%s", sb);
  s_sent_valid = true;
  s_sent_ms = now_ms;
  s_force = false;
}

void menu_can_describe(char *out, size_t n) {
  ElcCanMenuState st;
  char m[64], sb[64];
  current(st, m, sizeof(m), sb, sizeof(sb));
  static const char *const kPages[] = {"home", "reserved", "brightness", "theme", "screens", "service", "phone"};
  snprintf(out, n,
           "menu: %s on the %s screen (%s the interface screen), page %u %s, opened by %s\n"
           "  main \"%s\"\n"
           "  sub  \"%s\"\n"
           "  last sent: %s, text serial %u, %lu ms ago%s",
           st.open ? "open" : "closed", ui_screen_name(st.screen), interface_here() ? "is" : "not",
           (unsigned)st.page, st.page < 7 ? kPages[st.page] : "?",
           st.opened_by == ELC_CAN_MENU_BY_CONSOLE ? "console" : "touch", m, sb,
           s_sent_valid ? (s_sent.open ? "open" : "closed") : "nothing", (unsigned)s_serial,
           (unsigned long)(s_sent_valid ? millis() - s_sent_ms : 0), C.send ? "" : " (no CAN)");
}

bool menu_can_command(const char *line, char *reply, size_t n) {
  if (strcmp(line, "menu") == 0) {
    menu_can_describe(reply, n);
    return true;
  }
  ElcCanMenuInput in;
  in.from_node = ELC_CAN_NODE_CONSOLE;
  if (strncmp(line, "menu rotate ", 12) == 0) {
    in.event = ELC_CAN_MENU_ROTATE;
    in.value = (int8_t)atoi(line + 12);
  } else if (strcmp(line, "menu press") == 0) {
    in.event = ELC_CAN_MENU_PRESS;
  } else if (strcmp(line, "menu back") == 0) {
    in.event = ELC_CAN_MENU_BACK;
  } else if (strncmp(line, "menu open ", 10) == 0) {
    in.event = ELC_CAN_MENU_OPEN;
    in.value = (int8_t)atoi(line + 10);
  } else {
    return false;
  }
  queue(in);
  snprintf(reply, n, "ok: %s (as from the console%s)", line + 5,
           interface_here() ? "" : "; ignored: this is not the interface screen");
  return true;
}
