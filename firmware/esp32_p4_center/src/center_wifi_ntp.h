#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <stdint.h>

void center_wifi_ntp_begin(Preferences &prefs);

// POSIX TZ when prefs key "tz" is missing (UTC / GMT0).
#define CENTER_TZ_DEFAULT "GMT0"

// Apply stored TZ (POSIX) from prefs key "tz", default CENTER_TZ_DEFAULT.
void center_wifi_reload_tz(Preferences &prefs);

// Save POSIX TZ and apply (e.g. "GMT0", "GMT-4", "EST5EDT,M3.2.0/2,M11.1.0/2").
void center_wifi_set_tz_posix(Preferences &prefs, const char *posix);

// Whole-hour offset vs UTC (e.g. +4 → GMT-4 in Espressif-style POSIX). No DST rules.
void center_wifi_set_tz_utc_offset_hours(Preferences &prefs, int utc_offset_hours);

// STA connect using prefs wifi_ssid / wifi_psk (may block briefly).
void center_wifi_try_connect_saved();

void center_wifi_ntp_loop();

// After WiFi connects: SNTP once (restarts if WiFi reconnects).
void center_ntp_after_wifi_connected();

// true if gettimeofday looks like NTP has run (roughly post-2023).
bool center_wall_epoch_ms(uint64_t *out_ms);

// Blocking scan; returns count (<= max_n). out_enc[i] is WiFi.encryptionType raw byte.
int center_wifi_scan(char out_ssid[][33], uint8_t *out_enc, int max_n);

void center_wifi_scan_async_start();
// 0 busy, 1 done (*out_n set), 2 error/empty
int center_wifi_scan_async_poll(char out_ssid[][33], uint8_t *out_enc, int max_n, int *out_n);

void center_wifi_join_async_start(Preferences &prefs, const char *ssid, const char *psk);
void center_wifi_join_saved_async_start(Preferences &prefs);
// 0 in progress, 1 connected (prefs saved), 2 failed
int center_wifi_join_async_poll();

// Join and persist to prefs on success.
bool center_wifi_join_and_save(Preferences &prefs, const char *ssid, const char *psk);

void center_wifi_forget(Preferences &prefs);

// USB setup: lines like "wifi MySSID mypassword", "tz PST8PDT,M3.2.0,M11.1.0/2", "help"
void center_shell_poll(Preferences &prefs);

// While set, shell accepts "psk somesecret" to finish WPA join for pending_ssid.
void center_shell_set_wifi_psk_pending(const char *pending_ssid);
