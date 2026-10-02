#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <stdint.h>

// Wall time: UTC comes from GPS (settimeofday); the local zone is the POSIX TZ in prefs key "tz".
void center_time_begin(Preferences &prefs);

// NVS default until a zone is set (shell `tz ...`).
#define CENTER_TZ_DEFAULT "GMT0"

// Apply prefs key "tz" (POSIX) to libc; default CENTER_TZ_DEFAULT.
void center_time_reload_tz(Preferences &prefs);

// Full POSIX string, e.g. "CST6CDT,M3.2.0,M11.1.0" (DST rules work offline).
void center_time_set_tz_posix(Preferences &prefs, const char *posix);
// Fixed offset, no DST: -6 = UTC−6.
void center_time_set_tz_utc_offset_hours(Preferences &prefs, int utc_offset_hours);

// Time heard on the bus (a USB relay, later a gateway card), used while GPS has no fix: sets the clock, and the
// zone too when none is saved (not stored: the next boot is back to the saved zone or GMT).
void center_time_from_bus(uint32_t utc, int16_t offset_min);

// true if gettimeofday looks plausible (GPS settimeofday; roughly post-2023).
bool center_wall_epoch_ms(uint64_t *out_ms);
