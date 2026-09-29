#pragma once

#include <stdint.h>

// The two ways the unified display firmware runs (main.cpp picks one from display_role.h).

/** Center: tach / clock / media / efficiency, the menu host, Wi-Fi, GPS, SD, BLE, camera, cluster settings. */
void center_setup();
void center_loop();

/** Left (speedometer) or right (fuel / oil / temp / charge): gauges plus the menu and media over CAN. */
void side_setup(uint8_t role);
void side_loop();
