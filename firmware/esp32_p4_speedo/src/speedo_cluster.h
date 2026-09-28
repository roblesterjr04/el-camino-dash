#pragma once

#include <Arduino_GFX_Library.h>

void speedo_render(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, float mph, float odometer_miles,
                   bool sensor_pack_odometer_valid, const char *uart_banner = nullptr, bool data_fresh = true);
