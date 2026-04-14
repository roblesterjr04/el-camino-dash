#pragma once

#include <Arduino_GFX_Library.h>
#include <stdint.h>

#include <cluster_i2c.h>

// Classic: tach / clock / media. Mode changes via 3 s hold (classic) or menu.
enum class CenterViewMode : uint8_t { Tach = 0, Clock = 1, Media = 2 };

struct CenterGaugeInputs {
  uint32_t engine_rpm;
  uint32_t speed_rpm;
  uint32_t millis_boot;
  uint64_t epoch_ms;
  bool clock_wall;
  float fuel_pct;
  float coolant_temp_f;
  float system_amp;
  float trip_mi;
  float odometer_mi;
  int gear_num;
};

// When mode is Clock: if clock_use_wall_time and epoch_ms is plausible (NTP synced), hands follow wall time;
// otherwise millis_boot drives a boot-time clock (legacy behavior).
void center_render(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, CenterViewMode mode, ClusterTheme theme,
                   const CenterGaugeInputs *inputs);
