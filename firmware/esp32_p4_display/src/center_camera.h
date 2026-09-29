#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Backup camera on the center display.
 *
 * A TVP5150 module decodes the composite camera signal to 8-bit BT.656 (sync codes embedded in the
 * data), which the ESP32-P4 camera (DVP) input captures field by field. A task on the other core finds
 * the active lines, converts YUV 4:2:2 to RGB565 and scales to 640×480 (the largest 4:3 picture that fits
 * the round glass); the main loop shows it full screen with parking guide lines.
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

void center_camera_set_mode(CameraMode mode);

/** One status line for the console (`cam`). */
void center_camera_status(char *out, size_t n);

/** Bench tuning: invert the VSYNC or PCLK input if the capture does not frame (`cam vsync` / `cam pclk`). */
void center_camera_toggle_vsync_invert();
void center_camera_toggle_pclk_invert();
