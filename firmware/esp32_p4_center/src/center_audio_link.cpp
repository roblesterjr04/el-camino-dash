#include "center_audio_link.h"
#include "center_ui.h"

#include <Arduino.h>
#include <Wire.h>
#include <cstdio>
#include <cstring>

#ifndef CENTER_AUDIO_I2C_ADDR
#define CENTER_AUDIO_I2C_ADDR 0x2A
#endif
#ifndef CENTER_AUDIO_REG_METADATA
#define CENTER_AUDIO_REG_METADATA 0x01
#endif
#ifndef CENTER_AUDIO_REG_TRANSPORT
#define CENTER_AUDIO_REG_TRANSPORT 0x05
#endif
#ifndef CENTER_AUDIO_REG_SNAPSHOT
#define CENTER_AUDIO_REG_SNAPSHOT 0x0F
#endif
#ifndef CENTER_AUDIO_SNAPSHOT_BYTES
#define CENTER_AUDIO_SNAPSHOT_BYTES 48u
#endif
#ifndef CENTER_AUDIO_POLL_MS
#define CENTER_AUDIO_POLL_MS 500u
#endif
#ifndef CENTER_AUDIO_SENSOR_LOG_INTERVAL_MS
#define CENTER_AUDIO_SENSOR_LOG_INTERVAL_MS 5000u
#endif

static char s_line[40];
static bool s_ok;
static uint32_t s_last_meta_poll;

static uint16_t s_elapsed_s;
static uint16_t s_duration_s;
static uint8_t s_vol_0_15;
static bool s_tr_valid;

/** One slow line in MORE → SERIAL (sensor UART scrolls fast). */
static uint32_t s_last_audio_sensor_summary_ms;

/** Last I2C read sizes for the slow summary line (diagnose short reads vs empty slave buffer). */
static int s_last_tr_rx;
static int s_last_meta_rx;
static int s_last_meta_need;

static uint16_t elc565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | ((uint16_t)b >> 3));
}

static void drain_wire() {
  while (Wire.available()) {
    (void)Wire.read();
  }
}

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

/**
 * Single I2C read: metadata + transport packed by esp32_audio_bridge (reg 0x0F).
 * Layout: [mlen:1][meta:32][0xA5][5][e0][e1][d0][d1][vol][pad:8]
 */
static bool poll_snapshot() {
  Wire.beginTransmission(CENTER_AUDIO_I2C_ADDR);
  Wire.write(CENTER_AUDIO_REG_SNAPSHOT);
  if (Wire.endTransmission(true) != 0) {
    s_last_tr_rx = -1;
    return false;
  }
  delayMicroseconds(100);
  const int n = Wire.requestFrom((int)CENTER_AUDIO_I2C_ADDR, (int)CENTER_AUDIO_SNAPSHOT_BYTES);
  s_last_tr_rx = n;
  s_last_meta_rx = n;
  if (n < 40) {
    drain_wire();
    return false;
  }

  const int mlen_i = Wire.read();
  if (mlen_i < 0) {
    drain_wire();
    return false;
  }
  size_t mlen = (size_t)mlen_i;
  if (mlen > 32) {
    mlen = 32;
  }

  char meta[36];
  for (size_t i = 0; i < 32; i++) {
    const int c = Wire.read();
    if (c < 0) {
      drain_wire();
      return false;
    }
    meta[i] = (char)c;
  }
  meta[32] = '\0';

  const int magic = Wire.read();
  const int trhdr = Wire.read();
  const int e0 = Wire.read();
  const int e1 = Wire.read();
  const int d0 = Wire.read();
  const int d1 = Wire.read();
  const int vv = Wire.read();
  if (magic < 0 || trhdr < 0 || e0 < 0 || e1 < 0 || d0 < 0 || d1 < 0 || vv < 0) {
    drain_wire();
    return false;
  }
  drain_wire();

  if (magic != 0xA5 || trhdr != 5) {
    return false;
  }

  if (mlen > 0) {
    meta[mlen] = '\0';
    char tmp[40];
    strncpy(tmp, meta, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    if (trim_and_has_visible(tmp)) {
      strncpy(s_line, tmp, sizeof(s_line) - 1);
      s_line[sizeof(s_line) - 1] = '\0';
    }
  }
  s_ok = true;

  s_elapsed_s = (uint16_t)((uint16_t)e0 | ((uint16_t)e1 << 8));
  s_duration_s = (uint16_t)((uint16_t)d0 | ((uint16_t)d1 << 8));
  s_vol_0_15 = (uint8_t)vv;
  if (s_vol_0_15 > 15) {
    s_vol_0_15 = 15;
  }
  s_tr_valid = true;
  return true;
}

static bool poll_transport() {
  Wire.beginTransmission(CENTER_AUDIO_I2C_ADDR);
  Wire.write(CENTER_AUDIO_REG_TRANSPORT);
  if (Wire.endTransmission(true) != 0) {
    s_last_tr_rx = -1;
    return false;
  }
  delayMicroseconds(80);
  const int n = Wire.requestFrom((int)CENTER_AUDIO_I2C_ADDR, 6);
  s_last_tr_rx = n;
  if (n < 6) {
    drain_wire();
    return false;
  }
  const int len_byte = Wire.read();
  if (len_byte != 5) {
    drain_wire();
    return false;
  }
  const int e0 = Wire.read();
  const int e1 = Wire.read();
  const int d0 = Wire.read();
  const int d1 = Wire.read();
  const int vv = Wire.read();
  if (e0 < 0 || e1 < 0 || d0 < 0 || d1 < 0 || vv < 0) {
    drain_wire();
    return false;
  }
  drain_wire();
  s_elapsed_s = (uint16_t)((uint16_t)e0 | ((uint16_t)e1 << 8));
  s_duration_s = (uint16_t)((uint16_t)d0 | ((uint16_t)d1 << 8));
  s_vol_0_15 = (uint8_t)vv;
  if (s_vol_0_15 > 15) {
    s_vol_0_15 = 15;
  }
  s_tr_valid = true;
  return true;
}

/**
 * Bridge sends: [len:1][payload:len]. Master may request 33 bytes; n is actual bytes received.
 */
static bool read_metadata_block(char *out, size_t out_cap) {
  if (out_cap < 33) {
    return false;
  }
  Wire.beginTransmission(CENTER_AUDIO_I2C_ADDR);
  Wire.write(CENTER_AUDIO_REG_METADATA);
  if (Wire.endTransmission(true) != 0) {
    s_last_meta_rx = -1;
    s_last_meta_need = 0;
    return false;
  }
  delayMicroseconds(150);

  const int n = Wire.requestFrom((int)CENTER_AUDIO_I2C_ADDR, 33);
  s_last_meta_rx = n;
  if (n < 1) {
    drain_wire();
    s_last_meta_need = 0;
    return false;
  }

  const int len_byte = Wire.read();
  if (len_byte < 0) {
    drain_wire();
    s_last_meta_need = 0;
    return false;
  }

  size_t len = (size_t)len_byte;
  if (len > 32) {
    len = 32;
  }

  if (len == 0) {
    s_last_meta_need = 1;
    out[0] = '\0';
    drain_wire();
    return true;
  }

  const int need = 1 + (int)len;
  s_last_meta_need = need;
  if (n < need) {
    drain_wire();
    return false;
  }

  for (size_t i = 0; i < len; i++) {
    const int c = Wire.read();
    if (c < 0) {
      out[0] = '\0';
      drain_wire();
      return false;
    }
    out[i] = (char)c;
  }
  out[len] = '\0';
  drain_wire();
  return true;
}

void center_audio_link_begin() {
  s_line[0] = '\0';
  s_ok = false;
  s_last_meta_poll = 0;
  s_elapsed_s = 0;
  s_duration_s = 0;
  s_vol_0_15 = 0;
  s_tr_valid = false;
  s_last_audio_sensor_summary_ms = 0;
  s_last_tr_rx = -1;
  s_last_meta_rx = -1;
  s_last_meta_need = 0;
  Wire.setTimeOut(200);
  (void)Wire.setBufferSize(128);
}

void center_audio_link_poll() {
  const uint32_t now = millis();

  if (poll_snapshot()) {
    /* All state updated. */
  } else {
    /* Older bridge without 0x0F snapshot — separate metadata + transport (fragile on some buses). */
    if (now - s_last_meta_poll >= CENTER_AUDIO_POLL_MS) {
      s_last_meta_poll = now;

      bool meta_got = false;
      char raw[40];
      for (int attempt = 0; attempt < 5; attempt++) {
        if (read_metadata_block(raw, sizeof(raw))) {
          meta_got = true;
          if (raw[0] != '\0') {
            char tmp[40];
            strncpy(tmp, raw, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            if (trim_and_has_visible(tmp)) {
              strncpy(s_line, tmp, sizeof(s_line) - 1);
              s_line[sizeof(s_line) - 1] = '\0';
            }
          }
          break;
        }
        delayMicroseconds(250 + (uint32_t)attempt * 200);
      }
      if (meta_got) {
        s_ok = true;
      } else if (s_line[0] == '\0') {
        s_ok = false;
      }
    }
    (void)poll_transport();
  }

  if (now - s_last_audio_sensor_summary_ms >= CENTER_AUDIO_SENSOR_LOG_INTERVAL_MS) {
    s_last_audio_sensor_summary_ms = now;
    char logbuf[120];
    const bool tr_ok = s_tr_valid;
    if (!tr_ok) {
      snprintf(logbuf, sizeof(logbuf), "audio 0x2A: TR_-- (tr_rx=%d) meta=%s", s_last_tr_rx, s_ok ? "last_ok" : "fail");
    } else if (!s_ok) {
      if (s_last_meta_rx >= 0 && s_last_meta_need > 0 && s_last_meta_rx < s_last_meta_need) {
        snprintf(logbuf, sizeof(logbuf), "audio 0x2A: TR_ok  META I2C short read (rx=%d need=%d)", s_last_meta_rx,
                 s_last_meta_need);
      } else {
        snprintf(logbuf, sizeof(logbuf), "audio 0x2A: TR_ok  META I2C fail (rx=%d)", s_last_meta_rx);
      }
    } else if (trim_and_has_visible(s_line)) {
      snprintf(logbuf, sizeof(logbuf), "audio 0x2A: TR_ok  \"%.*s\"  %us/%us v%u", 52, s_line, (unsigned)s_elapsed_s,
               (unsigned)s_duration_s, (unsigned)s_vol_0_15);
    } else {
      snprintf(logbuf, sizeof(logbuf),
               "audio 0x2A: TR_ok  META no title (len=0) — flash esp32_audio_bridge w/ 0x0F snapshot");
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

void center_audio_link_draw(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h) {
  if (!gfx || !s_ok || s_line[0] == '\0') {
    return;
  }

  gfx->setFont(nullptr);
  gfx->setTextSize(1);
  int16_t x0 = 0, y0 = 0;
  uint16_t tw = 0, th = 0;
  gfx->getTextBounds(s_line, 0, 0, &x0, &y0, &tw, &th);
  const int16_t pad_x = 10;
  const int16_t pad_y = 6;
  const int16_t bar_h = (int16_t)(th + 2 * pad_y);
  const int16_t bar_y = (int16_t)(screen_h - bar_h - 6);
  if (bar_y < 0) {
    return;
  }
  const int16_t bar_w = screen_w;
  gfx->fillRect(0, bar_y, bar_w, bar_h, elc565(12, 12, 18));
  gfx->drawFastHLine(0, bar_y, bar_w, elc565(55, 55, 70));

  gfx->setTextColor(elc565(210, 210, 220));
  const int16_t tx = (int16_t)((screen_w - (int16_t)tw) / 2);
  const int16_t ty = (int16_t)(bar_y + pad_y - (int16_t)y0);
  gfx->setCursor(tx, ty);
  gfx->print(s_line);
}
