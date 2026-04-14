#pragma once

#include <Arduino_GFX_Library.h>

#include <sensor_packet.h>

void speedo_draw_fallout(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, float mph, const SensorDerived &d);
