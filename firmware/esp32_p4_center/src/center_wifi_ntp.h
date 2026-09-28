#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <stdint.h>

void center_wifi_ntp_begin(Preferences &prefs);

// NVS default until GPS+WiFi fills a POSIX TZ (see center_wifi_tz_build_posix_from_local_minus_utc).
#define CENTER_TZ_DEFAULT "GMT0"

// Apply prefs key "tz" (POSIX) to libc; default CENTER_TZ_DEFAULT.
void center_wifi_reload_tz(Preferences &prefs);

/** Build TZ string from API-style offset: seconds = local_wall − UTC (same instant). Uses the same
 *  `UTC±…` form as Arduino `configTime` / `setTimeZone` (newlib on ESP does not reliably honor `GEO4`-style
 *  POSIX abbreviations). Refetch periodically so DST tracks without zoneinfo. */
bool center_wifi_tz_build_posix_from_local_minus_utc(long local_minus_utc_sec, char *out, size_t out_cap);

void center_wifi_set_tz_posix(Preferences &prefs, const char *posix);
void center_wifi_set_tz_utc_offset_hours(Preferences &prefs, int utc_offset_hours);

// STA connect using prefs wifi_ssid / wifi_psk (may block briefly).
void center_wifi_try_connect_saved();

void center_wifi_ntp_loop();

// Legacy hook after WiFi connects (SNTP disabled; time from GPS).
void center_ntp_after_wifi_connected();

// true if gettimeofday looks plausible (GPS settimeofday or other sync; roughly post-2023).
bool center_wall_epoch_ms(uint64_t *out_ms);

void center_wifi_scan_async_start();
// 0 busy, 1 done (*out_n set), 2 error/empty
int center_wifi_scan_async_poll(char out_ssid[][33], uint8_t *out_enc, int max_n, int *out_n);

void center_wifi_join_async_start(Preferences &prefs, const char *ssid, const char *psk);
void center_wifi_join_saved_async_start(Preferences &prefs);
// 0 in progress, 1 connected (prefs saved), 2 failed
int center_wifi_join_async_poll();

void center_wifi_forget(Preferences &prefs);

// USB setup: lines like "wifi MySSID mypassword", "help"
void center_shell_poll(Preferences &prefs);

// While set, shell accepts "psk somesecret" to finish WPA join for pending_ssid.
void center_shell_set_wifi_psk_pending(const char *pending_ssid);
