#include "center_audio_link.h"
#include "center_can.h"
#include "center_ui.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <media_can.h>

#ifndef CENTER_AUDIO_SENSOR_LOG_INTERVAL_MS
#define CENTER_AUDIO_SENSOR_LOG_INTERVAL_MS 5000u
#endif

static char s_line[40];
static bool s_ok;

static uint16_t s_elapsed_s;
static uint16_t s_duration_s;
static uint8_t s_vol_0_15;
static bool s_tr_valid;

/** One slow line in MORE → SERIAL (sensor UART scrolls fast). */
static uint32_t s_last_audio_sensor_summary_ms;

/** Trim in place; returns true if any non-whitespace character remains. */
static bool trim_and_has_visible(char *s) {
  if (!s || !s[0]) {
    return false;
  }
  char *p = s;
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  if (p != s) {
    memmove(s, p, strlen(p) + 1u);
  }
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
    s[--n] = '\0';
  }
  return s[0] != '\0';
}

void center_audio_link_begin() {
  s_line[0] = '\0';
  s_ok = false;
  s_elapsed_s = 0;
  s_duration_s = 0;
  s_vol_0_15 = 0;
  s_tr_valid = false;
  s_last_audio_sensor_summary_ms = 0;
}

// ---------------------------------------------------------------- album art
//
// In the car, album art comes over CAN in two steps (media_can, shared with the other screens): a small copy first,
// then the phone's original. The center fetches it only while it is the media screen.

static bool s_fetch_art = true;

void center_audio_link_set_fetch_art(bool on) { s_fetch_art = on; }

void center_audio_art_status(char *out, size_t n) { media_can_art_status(out, n); }

bool center_audio_art(const uint16_t **px, int *w, int *h, int *stride, uint32_t *serial) {
  return media_art(px, w, h, stride, serial);
}

void center_audio_link_poll() {
  const uint32_t now = millis();

  // CAN: the link in the car. The bridge in its DigiDash slot sends text + transport and serves album art.
  char can_line[sizeof(s_line)];
  uint16_t can_el = 0, can_dur = 0;
  uint8_t can_vol = 0;
  const bool can = media_can_now_playing(can_line, sizeof(can_line), &can_el, &can_dur, &can_vol);
  if (can) {
    if (trim_and_has_visible(can_line)) {
      snprintf(s_line, sizeof(s_line), "%s", can_line);
    } else {
      s_line[0] = '\0';  // phone disconnected (the bridge clears its text)
    }
    s_elapsed_s = can_el;
    s_duration_s = can_dur;
    s_vol_0_15 = can_vol;
    s_tr_valid = true;
    s_ok = true;
  } else {
    s_ok = false;  // no bridge heard on CAN for 2 s
    s_tr_valid = false;
  }
  media_can_poll(s_fetch_art);

  if (now - s_last_audio_sensor_summary_ms >= CENTER_AUDIO_SENSOR_LOG_INTERVAL_MS) {
    s_last_audio_sensor_summary_ms = now;
    char logbuf[120];
    const char *src = "audio CAN";
    if (!s_ok || !s_tr_valid) {
      snprintf(logbuf, sizeof(logbuf), "%s: no bridge heard", src);
    } else if (trim_and_has_visible(s_line)) {
      snprintf(logbuf, sizeof(logbuf), "%s: \"%.*s\"  %us/%us v%u", src, 52, s_line, (unsigned)s_elapsed_s,
               (unsigned)s_duration_s, (unsigned)s_vol_0_15);
    } else {
      snprintf(logbuf, sizeof(logbuf), "%s: connected, nothing playing", src);
    }
    center_ui_sensor_log_line(logbuf);
  }
}

bool center_audio_link_ok() { return s_ok; }


const char *center_audio_link_line() { return s_line; }

bool center_audio_link_transport(uint16_t *elapsed_s, uint16_t *duration_s, uint8_t *vol_0_15) {
  if (elapsed_s) {
    *elapsed_s = s_elapsed_s;
  }
  if (duration_s) {
    *duration_s = s_duration_s;
  }
  if (vol_0_15) {
    *vol_0_15 = s_vol_0_15;
  }
  return s_tr_valid;
}
