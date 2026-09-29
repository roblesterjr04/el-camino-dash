#pragma once

#include <stdint.h>

#include <elc_can_ids.h>

/**
 * One firmware for all three round displays. The DigiDash board tells each screen where it sits with a strap on
 * header pin 17 (GPIO4), read once at boot with the internal pulls:
 *
 *   J5 (left)    pin 17 → 1 kΩ → GND      reads low with the pull-up        → left
 *   J4 (right)   pin 17 → 1 kΩ → 3V3      reads high with the pull-down     → right
 *   J1 (center)  pin 17 open              follows the pull either way       → center
 *
 * The position picks the CAN node, the I2C slave address, the gauge face and which services run (the center alone
 * runs Wi-Fi, GPS, SD, BLE, camera and owns the settings). A board off the DigiDash board (bench) reads "open"
 * (center); `role left|center|right` on the serial console stores an override in NVS, `role auto` clears it.
 */

enum class DisplayStrap : uint8_t { Low, High, Open };

/** Read the strap pin (DISPLAY_ROLE_STRAP_GPIO, default 4) and leave it as a plain input. */
DisplayStrap display_role_read_strap();

/** Position: the NVS override if one is stored, else the strap. Also returns what the strap read. */
uint8_t display_role_resolve(DisplayStrap *strap_out = nullptr, bool *override_out = nullptr);

/** Store (0..2) or clear (0xFF) the bench override and restart. */
[[noreturn]] void display_role_set_override_and_restart(uint8_t role);

/** "left", "center", "right". */
const char *display_role_name(uint8_t role);
const char *display_strap_name(DisplayStrap s);
/** Parses "left" / "center" / "right" (also "l" / "c" / "r", "0" / "1" / "2") → 0..2, "auto" → 0xFF, else -1. */
int display_role_parse(const char *s);

/** CAN node of a position (ELC_CAN_NODE_SPEEDO / _CENTER / _TSPS). */
uint8_t display_role_can_node(uint8_t role);

/** Serial `role …` command, shared by the center shell and the side console. False when `line` is not one. */
bool display_role_command(const char *line);
