#pragma once

#include <Arduino_GFX_Library.h>

#include <sensor_packet.h>

// Draw TSPS cluster (classic_2: four subgauges; classic_1: fuel-only) into a square buffer then blit.
void tsps_render_cluster(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, const SensorDerived &s, bool classic2);
