#pragma once

#include <Arduino_GFX_Library.h>
#include <stdint.h>

#include <cluster_i2c.h>

// Classic: tach / clock / media / efficiency (MPG+DTE). Mode changes via 3 s hold (classic) or menu.
// Numeric values are persisted: keep Media = 2 for existing NVS "view" = 2 installs.
enum class CenterViewMode : uint8_t { Tach = 0, Clock = 1, Media = 2, Efficiency = 3 };

struct CenterGaugeInputs {
  /** Fresh sensor_pack data; when false gauges are drawn at rest and marked "NO DATA". */
  bool sensor_valid;
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
  /** False when the link is stale or the sensor pack reports its odometer as unknown. */
  bool odometer_valid;
  int gear_num;
  /** Trip average MPG (0..40+ for needle sweep); invalid if fuel_economy_valid is false. */
  float mpg_average;
  /** Instant MPG for sub-gauge needle. */
  float mpg_instant;
  /** Distance to empty in tenths of a mile (sensor_pack v4). */
  uint32_t dte_tenths_mile;
  bool fuel_economy_valid;
  /** Reverse-geocoded road name when GPS + Wi‑Fi are available (may be empty). */
  const char *current_street;
};

// When mode is Clock: if clock_use_wall_time and epoch_ms is plausible (GPS UTC / settimeofday), hands follow
// wall time; otherwise millis_boot drives a boot-time clock (legacy behavior).
void center_render(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, CenterViewMode mode, ClusterTheme theme,
                   const CenterGaugeInputs *inputs);

/** RGB565 + alpha8 src-over (e.g. dial sprites from PROGMEM). */
void center_blend_src_over(uint16_t *dst, const uint16_t *src565, const uint8_t *alpha, int n);

/** Scale square cw×cw framebuffer to min(sw,sh) and center-blit (same as tach/clock). */
void center_gauge_scale_blit(Arduino_GFX *gfx, int16_t sw, int16_t sh, uint16_t *fb, int cw);

/** Rotated needle sprite; user_angle_deg matches clock sprite convention (see center_cluster.cpp). */
void center_blend_rotated_needle_like_clock(uint16_t *fb, int cw, const uint16_t *src565, const uint8_t *alpha,
                                            int sw, int sh, float user_angle_deg, float pivot_x_frac,
                                            float pivot_y_frac);

/** Same as above but pivot hub at (hub_x_px, hub_y_px) in the cw×cw framebuffer (e.g. lowered media arc). */
void center_blend_rotated_needle_like_clock_at(uint16_t *fb, int cw, const uint16_t *src565, const uint8_t *alpha,
                                               int sw, int sh, float user_angle_deg, float pivot_x_frac,
                                               float pivot_y_frac, float hub_x_px, float hub_y_px);
