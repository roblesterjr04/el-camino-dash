#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Backup camera on the center display.
 *
 * A TVP5150 module decodes the composite camera signal to 8-bit BT.656 (sync codes embedded in the
 * data), which the ESP32-P4 camera (DVP) input captures field by field. A task on the other core finds
 * the active lines, converts the centre of each from YUV 4:2:2 to RGB565 at native size, and has the PPA
 * scale that up to fill the 800×800 round glass (the CPU scales if the PPA is unavailable); the main loop
 * shows it full screen with parking guide lines.
 *
 * Shown while reverse is engaged (GPIO25, active low, with a short hold after leaving reverse), or when
 * forced from the console. Pins and wiring: WIRING_PINOUT.md, "Backup camera".
 */

enum class CameraMode : uint8_t { Auto, On, Off };

/** Configure the reverse input and look for the TVP5150 (retried every few seconds if absent). */
void center_camera_begin();

/** Reverse debounce, show/hide, and hand the newest converted frame to the display. Call every loop. */
void center_camera_loop(uint32_t now_ms);

/** The camera view is on screen (the gauges underneath can pause). */
bool center_camera_showing();

/** The reverse input is on (debounced, with the short hold after leaving reverse), whatever the camera mode. */
bool center_camera_reverse();

void center_camera_set_mode(CameraMode mode);

/** One status line for the console (`cam`). */
void center_camera_status(char *out, size_t n);

/** Bench check (`cam timing`): shortest / longest time between field ends since the last call, and how many were short. */
void center_camera_field_timing(char *out, size_t n);

/** Bench check (`cam pins`): watch PCLK and VSYNC for 200 ms and report edges and time high on each. */
void center_camera_pin_edges(char *out, size_t n);

/**
 * Bench check (`cam bits`): analyse the next captured field for bad data lines (D0–D7): each bit's share of ones and
 * of changes, how many BT.656 timing codes are valid, and horizontal blanking (a fixed 80 10 80 10 …) compared bit by
 * bit. Writes several lines, each starting "cam bits:". Waits up to 500 ms for a field; false if capture is stopped.
 */
bool center_camera_bit_check(char *out, size_t n);

/** Bench tuning: invert the VSYNC or PCLK input if the capture does not frame (`cam vsync` / `cam pclk`). */
void center_camera_toggle_vsync_invert();
void center_camera_toggle_pclk_invert();
