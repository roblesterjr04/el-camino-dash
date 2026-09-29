#include "center_audio_link.h"
#include "center_can.h"
#include "center_ui.h"

#include <Arduino.h>
#include <Wire.h>
#include <cstdio>
#include <cstring>
#include <driver/jpeg_decode.h>

#ifndef CENTER_AUDIO_I2C_ADDR
#define CENTER_AUDIO_I2C_ADDR 0x2A
#endif
#ifndef CENTER_AUDIO_REG_SNAPSHOT
#define CENTER_AUDIO_REG_SNAPSHOT 0x0F
#endif
#ifndef CENTER_AUDIO_SNAPSHOT_BYTES
#define CENTER_AUDIO_SNAPSHOT_BYTES 48u
#endif
#ifndef CENTER_AUDIO_SNAPSHOT_MS
#define CENTER_AUDIO_SNAPSHOT_MS 250u
#endif
#ifndef CENTER_AUDIO_SENSOR_LOG_INTERVAL_MS
#define CENTER_AUDIO_SENSOR_LOG_INTERVAL_MS 5000u
#endif

static char s_line[40];
static bool s_ok;

static uint16_t s_elapsed_s;
static uint16_t s_duration_s;
static uint8_t s_vol_0_15;
static bool s_tr_valid;

// Album art as announced by the latest snapshot.
static uint8_t s_art_snap_id;
static uint16_t s_art_snap_len;
static uint8_t s_art_snap_state;  // 0 none, 1 loading, 2 ready

/** One slow line in MORE → SERIAL (sensor UART scrolls fast). */
static uint32_t s_last_audio_sensor_summary_ms;

/** Last I2C read sizes for the slow summary line (diagnose short reads vs empty slave buffer). */
static int s_last_tr_rx;
static int s_last_meta_rx;
static int s_last_meta_need;

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
  delayMicroseconds(1000);  // the bridge loads its reply when the register select arrives
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
  // Album art description (bridges from 2026-09 on): [0xC7][art_id][len0][len1][state]
  int art[5] = {-1, -1, -1, -1, -1};
  for (int &a : art) {
    a = Wire.available() ? Wire.read() : -1;
  }
  drain_wire();
  if (art[0] == 0xC7) {
    s_art_snap_id = (uint8_t)art[1];
    s_art_snap_len = (uint16_t)(art[2] | (art[3] << 8));
    s_art_snap_state = (uint8_t)art[4];
  } else {
    s_art_snap_id = 0;
    s_art_snap_state = 0;
  }

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

void center_audio_link_begin() {
  s_line[0] = '\0';
  s_ok = false;
  s_elapsed_s = 0;
  s_duration_s = 0;
  s_vol_0_15 = 0;
  s_tr_valid = false;
  s_last_audio_sensor_summary_ms = 0;
  s_last_tr_rx = -1;
  s_last_meta_rx = -1;
  s_last_meta_need = 0;
  Wire.setTimeOut(25);
  (void)Wire.setBufferSize(128);
}

// ---------------------------------------------------------------- album art
//
// The bridge fetches the phone's cover-art thumbnail (JPEG) and serves it in chunks: write
// [0x10][idx_lo][idx_hi], read [0xC7][art_id][idx_lo][idx_hi][n][120 data bytes]. One chunk is read
// per frame (~13 ms of I2C at 100 kHz, shared with touch) so the gauges keep moving; a 20 KB image
// takes about 3 s. Every chunk header is checked (the slave can answer with a stale reply), and the
// P4's hardware JPEG decoder turns the result into RGB565.

static constexpr uint8_t kArtReg = 0x10;
static constexpr int kArtChunk = 120;
static constexpr size_t kArtInCap = 48 * 1024;

static uint8_t *s_jpg = nullptr;       // download buffer (DMA-capable, from the JPEG driver)
static size_t s_jpg_cap = 0;
static uint8_t s_fetch_id = 0;         // image being downloaded (0 = none)
static uint16_t s_fetch_len = 0;
static uint16_t s_fetch_next = 0;      // next chunk index
static uint16_t s_fetch_fails = 0;
static uint8_t s_art_done_id = 0;      // last image decoded (or given up on)
static uint16_t *s_art_px = nullptr;   // decoded RGB565
static int s_art_w = 0, s_art_h = 0, s_art_stride = 0;
static uint32_t s_art_serial = 0;      // bumps per decoded image
static bool s_art_valid = false;
static jpeg_decoder_handle_t s_jpeg = nullptr;

static bool art_decode(const uint8_t *jpg, size_t len) {
  if (!s_jpeg) {
    jpeg_decode_engine_cfg_t ec = {};
    ec.timeout_ms = 200;
    if (jpeg_new_decoder_engine(&ec, &s_jpeg) != ESP_OK) {
      s_jpeg = nullptr;
      return false;
    }
  }
  jpeg_decode_picture_info_t info = {};
  if (jpeg_decoder_get_info(jpg, len, &info) != ESP_OK || info.width == 0 || info.height == 0 || info.width > 1024 ||
      info.height > 1024) {
    return false;
  }
  // The decoder writes whole MCUs: 16 px wide for 4:2:0 / 4:2:2, 8 for 4:4:4 / gray; 16 px tall for 4:2:0.
  const bool wide = info.sample_method == JPEG_DOWN_SAMPLING_YUV420 || info.sample_method == JPEG_DOWN_SAMPLING_YUV422;
  const int mw = wide ? 16 : 8, mh = info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
  const int aw = ((int)info.width + mw - 1) / mw * mw, ah = ((int)info.height + mh - 1) / mh * mh;
  jpeg_decode_memory_alloc_cfg_t mc = {};
  mc.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  size_t cap = 0;
  uint16_t *out = (uint16_t *)jpeg_alloc_decoder_mem((size_t)aw * ah * 2, &mc, &cap);
  if (!out) {
    return false;
  }
  jpeg_decode_cfg_t cfg = {};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t out_size = 0;
  if (jpeg_decoder_process(s_jpeg, &cfg, jpg, (uint32_t)len, (uint8_t *)out, (uint32_t)cap, &out_size) != ESP_OK) {
    free(out);
    return false;
  }
  free(s_art_px);  // nothing keeps a pointer to the old image: the face copies it on a new serial
  s_art_px = out;
  s_art_w = (int)info.width;
  s_art_h = (int)info.height;
  s_art_stride = aw;
  s_art_serial++;
  s_art_valid = true;
  return true;
}

/** Read one chunk of the image being fetched; decode when the last one is in. */
static void art_pump() {
  if (s_art_snap_id == 0 && s_art_valid) {
    s_art_valid = false;  // new track: no art, or its art is still on the way from the phone
    s_art_done_id = 0;
  }
  if (s_art_snap_state != 2 || s_art_snap_id == 0 || s_art_snap_id == s_art_done_id) {
    return;
  }
  if (s_fetch_id != s_art_snap_id) {
    if (!s_jpg) {
      jpeg_decode_memory_alloc_cfg_t mc = {};
      mc.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
      s_jpg = (uint8_t *)jpeg_alloc_decoder_mem(kArtInCap, &mc, &s_jpg_cap);
    }
    if (!s_jpg || s_art_snap_len == 0 || s_art_snap_len > s_jpg_cap) {
      s_art_done_id = s_art_snap_id;  // cannot take this one
      return;
    }
    s_fetch_id = s_art_snap_id;
    s_fetch_len = s_art_snap_len;
    s_fetch_next = 0;
    s_fetch_fails = 0;
  }
  Wire.beginTransmission(CENTER_AUDIO_I2C_ADDR);
  Wire.write(kArtReg);
  Wire.write((uint8_t)(s_fetch_next & 0xFF));
  Wire.write((uint8_t)(s_fetch_next >> 8));
  bool ok = Wire.endTransmission(true) == 0;
  if (ok) {
    delayMicroseconds(1000);  // the bridge loads its reply when the register select arrives
    const int n = Wire.requestFrom((int)CENTER_AUDIO_I2C_ADDR, 5 + kArtChunk);
    uint8_t h[5];
    for (int i = 0; i < 5; i++) {
      const int c = Wire.read();
      h[i] = (uint8_t)c;
      ok = ok && c >= 0;
    }
    const uint16_t idx = (uint16_t)(h[2] | (h[3] << 8));
    const size_t off = (size_t)s_fetch_next * kArtChunk;
    const int want = (int)((s_fetch_len - off) < (size_t)kArtChunk ? (s_fetch_len - off) : (size_t)kArtChunk);
    ok = ok && n >= 5 + kArtChunk && h[0] == 0xC7 && h[1] == s_fetch_id && idx == s_fetch_next && h[4] == want;
    if (ok) {
      for (int i = 0; i < want; i++) {
        s_jpg[off + i] = (uint8_t)Wire.read();
      }
      s_fetch_next++;
    }
    drain_wire();
  }
  if (!ok && ++s_fetch_fails > 80) {
    s_fetch_id = 0;  // give up; retried when the next snapshot still announces it
    return;
  }
  if ((size_t)s_fetch_next * kArtChunk >= s_fetch_len) {
    const bool decoded = art_decode(s_jpg, s_fetch_len);
    Serial.printf("[audio] album art #%u: %u bytes, %s %dx%d\n", (unsigned)s_fetch_id, (unsigned)s_fetch_len,
                  decoded ? "decoded" : "decode FAILED", s_art_w, s_art_h);
    s_art_done_id = s_fetch_id;
    s_fetch_id = 0;
  }
}

void center_audio_art_status(char *out, size_t n) {
  snprintf(out, n, "announced id=%u len=%u state=%u, fetching id=%u chunk %u (fails %u), decoded #%u %s %dx%d",
           (unsigned)s_art_snap_id, (unsigned)s_art_snap_len, (unsigned)s_art_snap_state, (unsigned)s_fetch_id,
           (unsigned)s_fetch_next, (unsigned)s_fetch_fails, (unsigned)s_art_done_id, s_art_valid ? "shown" : "none",
           s_art_w, s_art_h);
}

bool center_audio_art(const uint16_t **px, int *w, int *h, int *stride, uint32_t *serial) {
  if (!s_art_valid || !s_art_px) {
    return false;
  }
  *px = s_art_px;
  *w = s_art_w;
  *h = s_art_h;
  *stride = s_art_stride;
  *serial = s_art_serial;
  return true;
}

void center_audio_link_poll() {
  const uint32_t now = millis();
  art_pump();
  // Metadata changes slowly; polling every frame kept the shared touch/display I2C bus busy and a
  // missing or unpowered ESP32-D could stall rendering on each attempt.
  static uint32_t s_last_poll_ms = 0;
  if (s_last_poll_ms != 0 && now - s_last_poll_ms < CENTER_AUDIO_SNAPSHOT_MS) {
    return;
  }
  s_last_poll_ms = now;

  // Only ever ask for the snapshot (0x0F). The ESP32 I2C slave often answers a read with the reply
  // queued for the previous request; mixing 0x01 and 0x05 reads meant each got the other's bytes,
  // so transport never parsed and the progress stayed at zero. With one request type, a late reply
  // is still a snapshot. The bridge has served 0x0F since before this firmware.
  static uint32_t s_fail_since = 0;
  if (poll_snapshot()) {
    s_fail_since = 0;
  } else if (!s_fail_since) {
    s_fail_since = now;
  } else if (now - s_fail_since > 3000u) {
    s_ok = false;  // bridge gone
    s_tr_valid = false;
  }

  // A bridge in a DigiDash slot also sends now-playing over CAN. When it is heard there, its text and
  // transport win; album art still comes over I2C (the snapshot read above).
  char can_line[sizeof(s_line)];
  uint16_t can_el = 0, can_dur = 0;
  uint8_t can_vol = 0;
  if (center_can_audio(can_line, sizeof(can_line), &can_el, &can_dur, &can_vol)) {
    if (trim_and_has_visible(can_line)) {
      snprintf(s_line, sizeof(s_line), "%s", can_line);
    }
    s_elapsed_s = can_el;
    s_duration_s = can_dur;
    s_vol_0_15 = can_vol;
    s_tr_valid = true;
    s_ok = true;
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

int center_audio_link_last_rx() { return s_last_tr_rx; }

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
