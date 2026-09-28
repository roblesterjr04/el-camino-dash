#pragma once

#include <Preferences.h>
#include <stdint.h>

/**
 * Center menu, drawn with LVGL above the gauge layer (see p4_screen.h).
 *
 * Home is a vertical wheel: the selected item sits in the middle, neighbours curve away along the rim
 * and fade. Drag or tap a neighbour to move, tap the middle item to open it. Pages: Wi-Fi, Brightness,
 * Theme, Service. The whole menu is rebuilt in the current theme's colors and fonts each time it opens.
 */
enum class CenterMenuPage : uint8_t { Home, Wifi, Brightness, Theme, Service };

void center_menu_begin(Preferences *prefs);
void center_menu_open(CenterMenuPage page = CenterMenuPage::Home);
void center_menu_close();
bool center_menu_is_open();

/** Encoder-style navigation (future knob; also used by the console). */
void center_menu_rotate(int steps);
void center_menu_press();

/**
 * Hold feedback on the gauge screen: a ring fills while the finger is down (3 s = next view,
 * 10 s = next theme). Call every frame; active=false hides it.
 */
void center_menu_hold_feedback(uint32_t held_ms, bool active);
