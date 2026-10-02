#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * The cluster menu, drawn with LVGL above the gauge layer (see p4_screen.h). Any of the three round displays
 * can host it: the "interface screen" setting (owned by the center, sent on CAN 0x210) picks which one. The
 * menu itself only draws and handles touch/knob input; everything it shows or changes goes through a
 * UiMenuHost: the center implements it with its own state, the left and right displays with CAN (setting
 * requests to the center, state from 0x210 / 0x213).
 *
 * Home is a vertical wheel: the selected item sits in the middle, neighbours curve away along the rim.
 * Drag or tap to move, tap the middle item to open it. Pages: Brightness, Theme, Screens, Service, Phone,
 * Accessories (every accessory's own page from the UI channel: accessory_can.h).
 * The menu is rebuilt in the current theme's colours and fonts each time it opens.
 *
 * The Phone page (pairing, the paired list, voice assistant, redial) reads the audio bridge's phone state from
 * phone_can.h and sends its commands with phone_can_send(): that module is the same on every screen, so it is
 * not part of the host.
 */

/** Page numbers match the console's MENU_INPUT / MENU_STATE (ElcCanMenuPage): keep them stable. */
enum class UiMenuPage : uint8_t {
  Home,
  Reserved1,  // was Wi-Fi; ui_menu_open ignores it
  Brightness,
  Theme,
  Screens,
  Service,
  Phone,
  Accessories,
};
static const uint8_t kUiMenuPages = 8;
/** Rows on the Service page (UiMenuHost::service_row). */
static const int kUiServiceRows = 7;

/** Screens, as in the CAN settings (ELC_CAN_SCREEN_*). */
enum : uint8_t { kUiScreenLeft = 0, kUiScreenCenter = 1, kUiScreenRight = 2 };

/** Center views (CenterViewMode numbering). */
enum : uint8_t { kUiViewTach = 0, kUiViewClock = 1, kUiViewMedia = 2, kUiViewEcon = 3 };

struct UiMenuHost {
  /** Which screen this board is (kUiScreen*). */
  uint8_t self_screen = kUiScreenCenter;

  // ---- cluster settings
  uint8_t (*theme)() = nullptr;  // 0 Classic, 1 Fallout, 2 Modern
  void (*set_theme)(uint8_t) = nullptr;
  uint8_t (*center_view)() = nullptr;
  void (*set_center_view)(uint8_t) = nullptr;
  uint8_t (*interface_screen)() = nullptr;
  void (*set_interface_screen)(uint8_t) = nullptr;
  uint8_t (*media_screen)() = nullptr;
  void (*set_media_screen)(uint8_t) = nullptr;
  bool (*media_shown)() = nullptr;         // the media screen shows its media view
  void (*show_media)(bool) = nullptr;
  uint8_t (*brightness)() = nullptr;       // 10..100
  bool (*dim_with_lights)() = nullptr;
  void (*set_brightness)(uint8_t level, bool follow_lights, bool save) = nullptr;

  // ---- Service rows: 0 Sensor link, 1 Odometer, 2 Faults, 3 GPS, 4 Audio bridge, 5 Left screen, 6 Right screen.
  // level: 0 ok, 1 warn, 2 bad.
  void (*service_row)(int row, char *value, size_t n, int *level) = nullptr;
  bool (*demo)() = nullptr;
  void (*set_demo)(bool) = nullptr;

  /** Open a full-screen page that only the center has (2 sensor log, 3 GPS data; 1 is reserved, was Wi-Fi).
   *  On the center it opens locally; elsewhere it is requested on the center screen. */
  void (*open_center_page)(uint8_t which) = nullptr;
};

void ui_menu_begin(const UiMenuHost *host);
void ui_menu_open(UiMenuPage page = UiMenuPage::Home);
void ui_menu_close();
bool ui_menu_is_open();
UiMenuPage ui_menu_page();

/** Knob (console) navigation: turn moves the wheel, adjusts brightness or moves the Phone page's focus; press opens
 *  the menu / opens the wheel item / activates the Phone page's focused choice / goes home from other pages. */
void ui_menu_rotate(int steps);
void ui_menu_press();
/** Back one step: a Phone page sub-view to its list, a page to Home, Home closes the menu. */
void ui_menu_back();

/**
 * What the console's OLED shows for the menu (0x218 / 0x219), each at most 32 bytes (cut on a UTF-8 boundary):
 * main = the focused wheel item, or the page's key fact ("Brightness 80%", "Theme Modern", the Phone page's
 * focused choice); sub = that item's value / state, or a page hint ("Dims with headlights", "Connected · …").
 * Both are "" while the menu is closed.
 */
void ui_menu_describe(char *main_text, size_t n_main, char *sub_text, size_t n_sub);

/**
 * Hold feedback on the gauge screen: a ring fills to `progress` (0..1000) with `message` in a pill near the
 * bottom. Call every frame; active = false hides it. The board decides what its holds do.
 */
void ui_menu_hold_feedback(bool active, int32_t progress, const char *message);

/** Short name of a screen ("Left", "Center", "Right"). */
const char *ui_screen_name(uint8_t screen);
