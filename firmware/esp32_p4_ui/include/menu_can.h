#pragma once

#include <stddef.h>
#include <stdint.h>

#include <elc_can_codec.h>

/**
 * The console's side of the round-screen menu (docs/protocols/can_bus.md, "Console"), the same on every screen:
 *   - MENU_INPUT (0x216) from the console drives this board's menu (ui_menu.h) while this board is the interface
 *     screen: rotate, press, back (a page, then close), open on a page. Other screens ignore it.
 *   - The interface screen sends MENU_STATE (0x217) and the two texts (0x218 / 0x219, ui_menu_describe()) on change,
 *     every second while the menu is open, and once when it closes. It remembers whether the console or a touch
 *     opened the menu.
 *   - A phone asking to pair (the bridge's CONFIRM flag coming on) opens the menu's Phone page on the interface
 *     screen, where Pair / Reject are (unless may_open says not now, e.g. the reversing camera is up).
 *
 * Loop contract: pass every CAN frame to menu_can_frame(); call menu_can_poll() once per loop.
 */
struct MenuCanConfig {
  uint8_t self_screen = ELC_CAN_SCREEN_CENTER;  // ELC_CAN_SCREEN_*
  bool (*send)(const ElcCanFrame &f) = nullptr;
  /** This board is the interface screen now (0x210). */
  bool (*is_interface)() = nullptr;
  /** The menu may open by itself now (nullptr = always). */
  bool (*may_open)() = nullptr;
  /** Called before the console (or a pairing request) opens the menu, e.g. to leave the center's older full-screen
   *  pages (nullptr = nothing to do). */
  void (*before_open)() = nullptr;
};

void menu_can_begin(const MenuCanConfig &cfg);

/** Feed one received frame. True when it was a MENU_INPUT (queued; applied by menu_can_poll()). */
bool menu_can_frame(const ElcCanFrame &f);

/** Apply queued console input, open the Phone page for a pairing request, send the menu state. */
void menu_can_poll(uint32_t now_ms);

/** The 0x217 state and texts this board sends now (whether or not it is the interface screen). */
void menu_can_describe(char *out, size_t n);

/**
 * Console: `menu` (the state above) and `menu rotate <n>|press|back|open <page 0..6>` (acts as if the console
 * sent it). Returns false when the line is not one of these (`menu on …` is the center's screen setting).
 */
bool menu_can_command(const char *line, char *reply, size_t n);
