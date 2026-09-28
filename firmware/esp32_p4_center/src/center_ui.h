#pragma once

#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <p4_overlay_menu.h>

#include "center_cluster.h"

struct SensorPacketValues;

void center_ui_begin(Arduino_GFX *gfx, Preferences *prefs, P4OverlayMenu *menu);
/** Bench/console: simulate a finger on the screen for hold_ms (implemented in main.cpp). */
void center_inject_touch(int16_t x, int16_t y, uint32_t hold_ms);

// Call from main after each good sensor_pack frame (Serial1); MORE → SERIAL shows a rolling log.
void center_ui_on_sensor_packet(const SensorPacketValues &v);

/** Raw UART text lines (e.g. ODO_FRAM_* from sensor_pack); appended to the same Sensor UART log. */
void center_ui_on_sensor_uart_line(const char *line);

/** Same rolling log (MORE → SERIAL); use for non-UART sources such as I2C audio bridge status. */
void center_ui_sensor_log_line(const char *line);

void center_ui_loop();

void center_ui_on_touch_release(int16_t x, int16_t y);

/** Call each frame with touch position; drives home menu scroll drag. */
void center_ui_touch_sample(int16_t x, int16_t y, bool finger_down);

// Long-press handling: duration is finger-down time until release (ms). Replaces direct
// p4_touch_poll_release when using continuous sampling in main.
void center_ui_process_touch_gesture(uint32_t duration_ms, int16_t x, int16_t y);

void center_ui_sync_cluster_bus();

bool center_ui_overlay_open();

bool center_ui_fullscreen();

void center_ui_draw_overlay();

void center_ui_draw_fullscreen(Arduino_GFX *gfx);

CenterViewMode center_ui_view_mode();

ClusterTheme center_ui_cluster_theme();
