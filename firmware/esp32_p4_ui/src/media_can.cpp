#include "media_can.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO)
#include <Arduino.h>
#else
extern "C" uint32_t millis(void);
#endif

#if defined(ARDUINO) && defined(__has_include)
#if __has_include(<driver/jpeg_decode.h>)
#include <driver/jpeg_decode.h>
#define MEDIA_HW_JPEG 1
#endif
#endif
#ifndef MEDIA_HW_JPEG
#define MEDIA_HW_JPEG 0
#endif

#if defined(ARDUINO)
#define MEDIA_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define MEDIA_LOG(...) printf(__VA_ARGS__)
#endif

namespace {

bool (*s_send)(const ElcCanFrame &) = nullptr;

// ---- bridge state (only one bridge is expected; the newest one wins)
ElcCanAudioTextDecoder s_text;
ElcCanAudioTransport s_tr;
uint32_t s_tr_ms = 0;
bool s_have = false;
uint8_t s_slot = 0;

// ---- decoded album art
uint8_t *s_jpg = nullptr;  // download buffer
size_t s_jpg_cap = 0;
uint16_t *s_art_px = nullptr;
int s_art_w = 0, s_art_h = 0, s_art_stride = 0;
uint32_t s_art_serial = 0;
bool s_art_valid = false;
#if MEDIA_HW_JPEG
jpeg_decoder_handle_t s_jpeg = nullptr;
#endif

// ---- CAN fetch
ElcCanArtReceiver s_rx;
uint8_t s_done_id = 0;       // last image finished (or given up on)
uint32_t s_rx_ms = 0;        // last frame of the image being collected
uint32_t s_req_ms = 0;       // last ART_REQUEST
uint16_t s_req_have = 0;     // chunks we had at that request
uint8_t s_req_tries = 0;     // requests in a row that brought nothing new
bool s_art_from_can = false;

bool heard(uint32_t max_age_ms) { return s_have && millis() - s_tr_ms <= max_age_ms; }

/** Start fetching `variant` of art `id`. */
void step(uint8_t id, uint8_t variant) {
  s_rx.start(s_slot, id, variant, s_jpg, s_jpg_cap);
  s_rx_ms = s_req_ms = 0;
  s_req_have = 0;
  s_req_tries = 0;
}

/** After the 128 px copy (shown or not), step up to the original; after the original, stop. */
void next(uint8_t id) {
  if (s_rx.variant() == ELC_CAN_ART_JPEG128) {
    step(id, ELC_CAN_ART_ORIGINAL);
  } else {
    s_done_id = id;
    s_rx.reset();
  }
}

bool visible(const char *s) {
  for (; s && *s; s++) {
    if ((unsigned char)*s > ' ') return true;
  }
  return false;
}

}  // namespace

void media_can_begin(bool (*send)(const ElcCanFrame &f)) { s_send = send; }

bool media_can_frame(const ElcCanFrame &f) {
  uint8_t slot = 0;
  ElcCanAudioTransport t;
  if (elc_can_decode_audio_transport(f, &slot, t)) {
    s_tr = t;
    s_slot = slot;
    s_tr_ms = millis();
    s_have = true;
    return true;
  }
  if (s_text.feed(f)) {
    return true;
  }
  if (s_rx.feed(f)) {
    s_rx_ms = millis();
    return true;
  }
  return false;
}

bool media_can_now_playing(char *line, size_t n, uint16_t *elapsed_s, uint16_t *duration_s, uint8_t *vol_0_15,
                           uint32_t max_age_ms) {
  if (!heard(max_age_ms)) {
    return false;
  }
  if (line && n) {
    // Only a text that matches the transport's serial is current (a new track may still be arriving).
    const bool current = s_text.valid() && s_text.serial() == s_tr.text_serial;
    snprintf(line, n, "%s", current && s_tr.connected ? s_text.text() : "");
  }
  if (elapsed_s) *elapsed_s = s_tr.elapsed_s;
  if (duration_s) *duration_s = s_tr.duration_s;
  if (vol_0_15) *vol_0_15 = s_tr.volume > 15 ? 15 : s_tr.volume;
  return true;
}

uint8_t *media_art_buffer(size_t *cap) {
#if MEDIA_HW_JPEG
  if (!s_jpg) {
    jpeg_decode_memory_alloc_cfg_t mc = {};
    mc.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
    s_jpg = (uint8_t *)jpeg_alloc_decoder_mem(ELC_CAN_ART_MAX_BYTES, &mc, &s_jpg_cap);
  }
#else
  if (!s_jpg) {
    s_jpg = (uint8_t *)malloc(ELC_CAN_ART_MAX_BYTES);
    s_jpg_cap = s_jpg ? ELC_CAN_ART_MAX_BYTES : 0;
  }
#endif
  if (cap) *cap = s_jpg_cap;
  return s_jpg;
}

bool media_art_decode(const uint8_t *jpg, size_t len) {
#if MEDIA_HW_JPEG
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
#else
  (void)jpg;
  (void)len;
  return false;
#endif
}

void media_art_clear() { s_art_valid = false; }

bool media_art(const uint16_t **px, int *w, int *h, int *stride, uint32_t *serial) {
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

void media_can_poll(bool fetch_art) {
  if (!heard(2000)) {
    if (s_art_from_can) {  // bridge gone from CAN: drop its art
      media_art_clear();
      s_done_id = 0;
      s_rx.reset();
      s_art_from_can = false;
    }
    return;
  }
  s_art_from_can = true;
  const uint8_t id = s_tr.connected ? s_tr.art_id : 0;
  if (id == 0) {  // new track with no art (yet), or phone gone
    media_art_clear();
    s_done_id = 0;
    s_rx.reset();
    return;
  }
  if (!fetch_art) {
    if (s_rx.art_id() != 0) {
      s_rx.reset();  // stopped showing media: stop asking (a finished image stays for next time)
    }
    return;
  }
  if (id == s_done_id) {
    return;
  }
  if (s_rx.art_id() != id) {
    if (!media_art_buffer(nullptr)) {
      s_done_id = id;  // cannot take any image
      return;
    }
    if (s_art_valid) {
      media_art_clear();  // the previous track's picture
    }
    step(id, ELC_CAN_ART_JPEG128);  // fast small picture first
  }
  const char *vname = s_rx.variant() == ELC_CAN_ART_JPEG128 ? "128px" : "original";
  if (s_rx.refused()) {
    MEDIA_LOG("[audio] album art #%u %s over CAN: not available (size %u), %s\n", (unsigned)id, vname,
              (unsigned)s_rx.size(), s_rx.variant() == ELC_CAN_ART_JPEG128 ? "asking for the original" : "done");
    next(id);
    return;
  }
  if (s_rx.complete()) {
    const bool decoded = media_art_decode(s_jpg, s_rx.size());
    MEDIA_LOG("[audio] album art #%u %s over CAN: %u bytes, %s %dx%d\n", (unsigned)id, vname, (unsigned)s_rx.size(),
              decoded ? "decoded" : "decode FAILED", s_art_w, s_art_h);
    next(id);
    return;
  }
  // (Re)ask from the first missing chunk once the stream has gone quiet (frames are single-shot).
  const uint32_t now = millis();
  if (now - s_rx_ms > 300u && now - s_req_ms > 300u) {
    if (s_rx.chunks_have() > s_req_have) {
      s_req_tries = 0;  // the last request brought data: not stuck
    }
    if (++s_req_tries > 20) {
      MEDIA_LOG("[audio] album art #%u %s over CAN: no progress, given up at chunk %u\n", (unsigned)id, vname,
                (unsigned)s_rx.first_missing());
      next(id);  // a 128px picture already shown stays up
      return;
    }
    s_req_have = s_rx.chunks_have();
    s_req_ms = now;
    if (s_send) {
      ElcCanFrame f;
      elc_can_encode_art_request(ElcCanArtRequest{s_slot, id, s_rx.first_missing(), s_rx.variant()}, f);
      s_send(f);
    }
  }
}

void media_can_art_status(char *out, size_t n) {
  snprintf(out, n, "CAN: collecting id=%u %s %u/%u chunks (size %u, tries %u), decoded #%u %s %dx%d",
           (unsigned)s_rx.art_id(), s_rx.variant() == ELC_CAN_ART_JPEG128 ? "128px" : "original",
           (unsigned)s_rx.chunks_have(), (unsigned)(s_rx.sized() ? elc_can_art_chunks(s_rx.size()) : 0),
           (unsigned)s_rx.size(), (unsigned)s_req_tries, (unsigned)s_done_id, s_art_valid ? "shown" : "none",
           s_art_w, s_art_h);
}

void media_can_fill(GaugeData &d, uint32_t now_ms) {
  static char line[40], artist[40], title[40];
  uint16_t el = 0, dur = 0;
  uint8_t vol = 0;
  d.media_connected = media_can_now_playing(line, sizeof(line), &el, &dur, &vol) && visible(line);
  if (!d.media_connected) {
    return;
  }
  const char *sep = strstr(line, " - ");
  if (sep) {
    const size_t k = (size_t)(sep - line) < sizeof(artist) - 1 ? (size_t)(sep - line) : sizeof(artist) - 1;
    memcpy(artist, line, k);
    artist[k] = '\0';
    snprintf(title, sizeof(title), "%s", sep + 3);
  } else {
    artist[0] = '\0';
    snprintf(title, sizeof(title), "%s", line);
  }
  d.media_title = title;
  d.media_artist = artist;
  uint32_t serial = 0;
  if (media_art(&d.art_px, &d.art_w, &d.art_h, &d.art_stride, &serial)) {
    d.art_id = serial;
  }
  static uint16_t s_el = 0xFFFF;
  static uint32_t s_el_ms = 0;
  if (dur > 0) {
    if (el != s_el) {
      s_el = el;
      s_el_ms = now_ms;
    }
    const float since = (float)(now_ms - s_el_ms) / 1000.0f;
    d.media_elapsed = (float)el + (since < 1.0f ? since : 1.0f);
    d.media_duration = (float)dur;
  }
}
