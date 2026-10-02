#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <stdint.h>

/** Secondary UART for NMEA from an external GPS (GPS TX → this RX). Often called “soft serial” in wiring
 *  guides; on ESP32 this is a hardware UART, not bit-banged SoftwareSerial. */
void center_gps_begin(Preferences &prefs);

/** Drain GPS UART and parse RMC/ZDA (UTC) into settimeofday. Non-blocking; call from main loop. */
void center_gps_loop();

/** millis() of last byte read from the GPS UART (0 if none yet). Use to confirm wiring/baud (NEO‑6M default 9600). */
uint32_t center_gps_last_uart_byte_ms();

/** True once we have lat/lon from RMC (status A) or GGA (fix quality 1–9). */
bool center_gps_have_position_fix();

uint32_t center_gps_uart_baud();

/** If a fix is stored, writes lat/lon and returns true. */
bool center_gps_last_fix_coords(double *out_lat, double *out_lon);

/** Current incomplete NMEA line (between sentence starts and CR/LF); not necessarily null-terminated internally. */
void center_gps_diag_copy_partial(char *out, size_t cap);

#ifndef CENTER_GPS_DIAG_RECENT_LINES
#define CENTER_GPS_DIAG_RECENT_LINES 15
#endif

/** Last `CENTER_GPS_DIAG_RECENT_LINES` complete `$` sentences, oldest at [0], newest at [N-1]. */
void center_gps_diag_copy_recent(char out_rows[CENTER_GPS_DIAG_RECENT_LINES][128]);
