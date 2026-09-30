#pragma once

#include <Preferences.h>
#include <stdint.h>

/**
 * The center's side of the cluster menu (esp32_p4_ui ui_menu.h): the menu is drawn by the shared module; this
 * file backs it with the center's own state (settings, Wi-Fi radio, service data) and keeps brightness.
 * The menu opens on the center on a tap only when it is the interface screen; another screen can still ask
 * for a center-only page (Wi-Fi keyboard, logs, phone) over CAN.
 */
enum class CenterMenuPage : uint8_t { Home, Wifi, Brightness, Theme, Screens, Service, Phone };

void center_menu_begin(Preferences *prefs);
void center_menu_open(CenterMenuPage page = CenterMenuPage::Home);
void center_menu_close();
bool center_menu_is_open();

/** Encoder-style navigation (bench `knob`; the console's knob when the center is the interface screen). */
void center_menu_rotate(int steps);
void center_menu_press();

/**
 * Hold feedback on the gauge screen: a ring fills while the finger is down (3 s = next view,
 * 10 s = next theme). Call every frame; active=false hides it.
 */
void center_menu_hold_feedback(uint32_t held_ms, bool active);

/** Brightness (NVS "bright" / "bfollow"), shared with the other screens in the cluster state. */
uint8_t center_menu_brightness();
bool center_menu_dim_with_lights();
void center_menu_set_brightness(uint8_t level, bool follow_lights, bool save);

/** Call every loop: finishes Wi-Fi joins even with the menu closed; the console's menu input and state (menu_can). */
void center_menu_loop();
/** Join the saved network (menu, or a request from another screen). */
void center_menu_wifi_join_saved();
bool center_menu_wifi_joining();

/** Open a center-only page (ui_menu.h open_center_page: 1 Wi-Fi, 2 sensor log, 3 GPS data). 4 (the old phone page,
 *  from older side-screen firmware) opens the menu's Phone page here. */
void center_menu_open_center_page(uint8_t which);
