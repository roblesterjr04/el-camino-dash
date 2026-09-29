#pragma once

#include <sensor_link.h>

/**
 * Bench console for the speedometer and right displays (Serial, 115200):
 *   shot            screenshot for tools/screenshot.py
 *   perf            frame timing, sensor link and heap
 *   demo on|off     simulated drive cycle instead of the sensor pack
 *   theme 0|1|2     Classic / Fallout / Modern (normally set by the center over I2C)
 *   variant 0|1     right display Classic layout
 *   can             CAN counters and the nodes heard
 */
struct GaugeConsole {
  void (*on_theme)(uint8_t theme) = nullptr;
  void (*on_variant)(uint8_t variant) = nullptr;
  void (*on_can)() = nullptr;
  void poll(SensorLink &link);
};
