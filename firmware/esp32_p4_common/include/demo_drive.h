#pragma once

#include <stdint.h>

#include <sensor_packet.h>

/**
 * Bench demo: a 60 s drive cycle (idle, pull away through three gears, cruise, pass, stop) that
 * produces the same decoded frames the sensor pack would send. Used by SensorLink::setDemo().
 */
class DemoDrive {
 public:
  void start(uint32_t now_ms, uint32_t odometer_tenths);
  void frame(uint32_t now_ms, SensorPacketValues &out);

 private:
  uint32_t t0_ms_ = 0;
  uint32_t last_ms_ = 0;
  uint32_t odo_tenths_ = 0;
  float odo_frac_ = 0.0f;
  float mph_ = 0.0f;
  float coolant_f_ = 150.0f;
  float fuel_pct_ = 64.0f;
  float mpg_avg_ = 17.5f;
};
