#pragma once

#include <Arduino.h>
#include <esp_task_wdt.h>

// Loop-task watchdog: if loop() stops returning (hung I2C transfer, render deadlock) the board reboots
// instead of showing a frozen gauge. Nothing in loop() may block longer than this — long work belongs
// in its own task.
#ifndef ELC_LOOP_WDT_TIMEOUT_MS
#define ELC_LOOP_WDT_TIMEOUT_MS 8000u
#endif

/** Call once at the end of setup(). */
inline void elc_loop_watchdog_begin() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = ELC_LOOP_WDT_TIMEOUT_MS;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
    esp_task_wdt_init(&cfg);
  }
  enableLoopWDT();  // the Arduino core feeds it after every loop() iteration
}

/** Optional extra feed inside a long-but-bounded loop() iteration. */
inline void elc_loop_watchdog_feed() { esp_task_wdt_reset(); }
