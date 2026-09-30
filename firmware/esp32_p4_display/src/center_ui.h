#pragma once

#include <Arduino_GFX_Library.h>
#include <Preferences.h>

#include <cluster_i2c.h>

#include "center_views.h"

struct SensorPacketValues;
class SensorLink;

void center_ui_begin(Arduino_GFX *gfx, Preferences *prefs);

/** Sensor link owned by main.cpp (read-only access for the Service page). */
const SensorLink &center_sensor_link();

enum class CenterLegacyPage : uint8_t { SensorLog, GpsDiag };
void center_ui_set_view(CenterViewMode v);    // persists + syncs the cluster bus
void center_ui_set_theme(ClusterTheme t);     // persists + syncs the cluster bus

// Screens (ELC_CAN_SCREEN_*), stored in NVS "iface" / "media" / "mshown" and the SD settings.json.
/** The screen with the on-screen menu (touch and the console knob). */
uint8_t center_ui_interface_screen();
void center_ui_set_interface_screen(uint8_t screen);
/** The screen with the media view. On the center, media is one of its views; on a side screen it is an extra
 *  view next to that screen's gauge (media_shown). */
uint8_t center_ui_media_screen();
void center_ui_set_media_screen(uint8_t screen);
bool center_ui_media_shown();
void center_ui_show_media(bool on);
void center_ui_open_legacy_page(CenterLegacyPage p);
/** Leave an older full-screen page (keyboard, logs) without opening anything: the menu is about to open. */
void center_ui_leave_fullscreen();
/** Right screen's Classic layout (0x212 setting 11, NVS "rlayout" + settings.json): true = fuel + lamps, sent as
 *  0x210 flag bit2; false = four gauges. */
bool center_ui_right_layout2();
void center_ui_set_right_layout2(bool fuel_and_lamps);
void center_ui_open_wifi_password(const char *ssid);
/** Bench/console: simulate a finger on the screen for hold_ms (implemented in main.cpp). */
void center_inject_touch(int16_t x, int16_t y, uint32_t hold_ms);
void center_inject_drag(int16_t x, int16_t y1, int16_t y2, uint32_t ms);
/** Bench/console: simulated drive cycle instead of sensor-pack data (implemented in main.cpp). */
void center_set_demo(bool on);

// Call from main after each good sensor_pack frame (Serial1); MORE → SERIAL shows a rolling log.
void center_ui_on_sensor_packet(const SensorPacketValues &v);

/** Raw UART text lines (e.g. ODO_FRAM_* from sensor_pack); appended to the same Sensor UART log. */
void center_ui_on_sensor_uart_line(const char *line);

/** Same rolling log (MORE → SERIAL); use for non-UART sources such as I2C audio bridge status. */
void center_ui_sensor_log_line(const char *line);

void center_ui_loop();

void center_ui_on_touch_release(int16_t x, int16_t y);

// Long-press handling: duration is finger-down time until release (ms). Replaces direct
// p4_touch_poll_release when using continuous sampling in main.
void center_ui_process_touch_gesture(uint32_t duration_ms, int16_t x, int16_t y);

void center_ui_sync_cluster_bus();

bool center_ui_overlay_open();

bool center_ui_fullscreen();

void center_ui_draw_fullscreen(Arduino_GFX *gfx);

CenterViewMode center_ui_view_mode();

ClusterTheme center_ui_cluster_theme();
/** Theme order for the 10 s hold: Classic → Modern → Fallout → Classic. */
ClusterTheme center_ui_next_theme(ClusterTheme t);
