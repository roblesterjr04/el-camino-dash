/*
 * esp32_audio_bridge.ino — in-dash Bluetooth media hub (ESP32 classic BT only)
 *
 * - A2DP sink: phone / portable player streams stereo audio to the ESP32 **internal** DAC
 *   (GPIO25 / GPIO26 — classic ESP32 only) by default; optional I2S → external DAC/amp via define.
 * - AVRCP 1.x: title, artist, album, track #, genre, duration (typical car-infotainment metadata).
 * - I2C slave @ 0x2A for the center ESP32-P4: it reads a 48-byte snapshot (register 0x0F) with the
 *   "now playing" line + elapsed/duration/volume. Registers 0x01 / 0x05 remain for older hosts.
 *
 * HARDWARE: Bluetooth Classic (A2DP) exists on the original ESP32 only — not ESP32-S3 / C3 / S2.
 * Use a module such as ESP32-WROOM-32 and set FQBN to esp32:esp32:esp32 when building.
 *
 * External I2S: wire BCK/WS/DOUT per I2S_PIN_* and build with AUDIO_BRIDGE_USE_INTERNAL_DAC=0
 * (see firmware/esp32_classic_bt_audio/platformio.ini env esp32dev_external_i2s).
 *
 * ESP32-P4 center (Waveshare): connect this chip's I2C to the **same bus** as the round
 * display — **SDA → P4 GPIO 7**, **SCL → P4 GPIO 8** (this chip: SDA=GPIO21, SCL=GPIO22).
 *
 * Threading: AVRCP callbacks (BT task) update state and rebuild a prebuilt reply buffer under a
 * spinlock; the I2C onRequest handler only copies that buffer, so it answers quickly.
 */

#include <Arduino.h>
#include <Wire.h>
#include <cstring>

#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include "cluster_theme_i2c.h"
#include "esp_avrc_api.h"

#include <cstdlib>

/* Set to 1 (or -D in platformio.ini) to print AVRCP-built line on Serial when it changes / every 3s. */
#ifndef AUDIO_BRIDGE_SERIAL_METADATA
#define AUDIO_BRIDGE_SERIAL_METADATA 0
#endif

#ifndef BT_DEVICE_NAME
#define BT_DEVICE_NAME "ElCamino-Media"
#endif

#ifndef I2C_SLAVE_ADDR
#define I2C_SLAVE_ADDR 0x2A
#endif

#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN SDA
#endif

#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN SCL
#endif

// Second I2C peripheral: cluster theme slave (center P4 master on GPIO 47/48 in vehicle).
// Metadata bridge uses `Wire` @ 0x2A; theme mirror uses `Wire1` @ 0x3E — different pins.
#ifndef CLUSTER_THEME_I2C_ENABLE
#define CLUSTER_THEME_I2C_ENABLE 1
#endif
#ifndef CLUSTER_THEME_SDA
#define CLUSTER_THEME_SDA 18
#endif
#ifndef CLUSTER_THEME_SCL
#define CLUSTER_THEME_SCL 19
#endif

// 1 (default) = built-in 8-bit DAC on GPIO25 (L) / GPIO26 (R). 0 = I2S → external DAC/amp — classic ESP32 only.
// This flag only selects AnalogAudioStream vs I2SStream + pins; AVRCP + I2C slave metadata are identical in both builds.
#ifndef AUDIO_BRIDGE_USE_INTERNAL_DAC
#define AUDIO_BRIDGE_USE_INTERNAL_DAC 1
#endif

// I2S to external DAC (override per your PCB; ignored when AUDIO_BRIDGE_USE_INTERNAL_DAC=1).
// Avoid GPIO 21/22 (I2C to the center) and strapping pins (0, 2, 5, 12, 15).
#ifndef I2S_PIN_BCK
#define I2S_PIN_BCK 14
#endif
#ifndef I2S_PIN_WS
#define I2S_PIN_WS 25
#endif
#ifndef I2S_PIN_DATA
#define I2S_PIN_DATA 27
#endif
#if I2S_PIN_DATA == 22 || I2S_PIN_WS == 22 || I2S_PIN_BCK == 22 || I2S_PIN_DATA == 21 || I2S_PIN_WS == 21 || \
    I2S_PIN_BCK == 21
#error "I2S pins collide with the I2C link to the center display (GPIO 21/22)"
#endif

static constexpr uint8_t kRegMetadata = 0x01;
static constexpr uint8_t kRegTransport = 0x05;
/** P4 center: one atomic read for title + transport (avoids s_i2c_cmd / onRequest races). Pi keeps using 0x01/0x05. */
static constexpr uint8_t kRegSnapshot = 0x0F;
static constexpr size_t kMaxTrackLen = 32;
/** Snapshot layout: [mlen][meta:32][0xA5][5][e0][e1][d0][d1][vol][pad:8] = 48 bytes. The metadata
 *  reply (0x01) is bytes 0..32 and the transport reply (0x05) is bytes 34..39 of the same buffer. */
static constexpr size_t kSnapshotLen = 48;
static constexpr size_t kSnapTransportOff = 34;
static constexpr size_t kTransportLen = 6;

static char s_track[kMaxTrackLen + 1];
static volatile size_t s_track_len;
static volatile uint8_t s_i2c_cmd = kRegMetadata;
static uint8_t s_snapshot[kSnapshotLen];

static uint16_t s_elapsed_s;
static uint16_t s_duration_s;
static uint8_t s_vol_0_15;

static char s_meta_artist[kMaxTrackLen + 1];
static char s_meta_title[kMaxTrackLen + 1];
static portMUX_TYPE s_track_mux = portMUX_INITIALIZER_UNLOCKED;

/* Elapsed estimate when the phone sends play-position notifications rarely (common): count only
 * time spent playing, starting from the last known position (s_pos_base_s at s_pos_anchor_ms). */
static uint32_t s_pos_anchor_ms;
static uint16_t s_pos_base_s;
static uint32_t s_last_play_pos_wall_ms;
static bool s_playing;
static char s_prev_track_for_elapsed[kMaxTrackLen + 1];

static void apply_elapsed_estimate_unlocked() {
  if (!s_playing || s_duration_s == 0) {
    return;
  }
  const uint32_t now = millis();
  if (now - s_last_play_pos_wall_ms < 2000u) {
    return;
  }
  uint32_t est = (uint32_t)s_pos_base_s + (now - s_pos_anchor_ms) / 1000u;
  if (est > (uint32_t)s_duration_s) {
    est = s_duration_s;
  }
  s_elapsed_s = (est > 65535u) ? 65535 : (uint16_t)est;
}

/** Re-anchor the estimate at the current elapsed value (on play/pause, position, track change). */
static void anchor_position_unlocked(uint16_t pos_s) {
  s_pos_base_s = pos_s;
  s_pos_anchor_ms = millis();
  s_elapsed_s = pos_s;
}

#if CLUSTER_THEME_I2C_ENABLE
/* The center broadcasts the cluster theme to 0x3E; nothing on this board uses it yet, so it is
 * only acknowledged (keeps the center's broadcast from logging NACKs). */
static void on_cluster_theme_receive(int) {
  while (Wire1.available()) {
    (void)Wire1.read();
  }
}
#endif

#if AUDIO_BRIDGE_USE_INTERNAL_DAC
// AnalogAudioStream drives I2S_MODE_DAC_BUILT_IN, sets DAC channel mode, and offsets PCM for the ROM DAC.
AnalogAudioStream audio_output;
#else
I2SStream audio_output;
#endif
BluetoothA2DPSink a2dp_sink(audio_output);

static void rebuild_track_line() {
  s_meta_artist[kMaxTrackLen] = '\0';
  s_meta_title[kMaxTrackLen] = '\0';

  const char *a = s_meta_artist[0] ? s_meta_artist : "";
  const char *t = s_meta_title[0] ? s_meta_title : "";
  if (a[0] && t[0]) {
    const size_t la = strnlen(a, kMaxTrackLen);
    const size_t lt = strnlen(t, kMaxTrackLen);
    if (la + 3u + lt <= kMaxTrackLen) {
      snprintf(s_track, sizeof(s_track), "%s - %s", a, t);
    } else {
      strncpy(s_track, t, kMaxTrackLen);
      s_track[kMaxTrackLen] = '\0';
    }
  } else if (t[0]) {
    strncpy(s_track, t, kMaxTrackLen);
    s_track[kMaxTrackLen] = '\0';
  } else if (a[0]) {
    strncpy(s_track, a, kMaxTrackLen);
    s_track[kMaxTrackLen] = '\0';
  } else {
    s_track[0] = '\0';
  }
  s_track_len = strnlen(s_track, kMaxTrackLen);
}

/** Rebuild the I2C reply buffer from current state. Caller holds s_track_mux. */
static void rebuild_snapshot_unlocked() {
  apply_elapsed_estimate_unlocked();
  const uint8_t mlen = static_cast<uint8_t>((s_track_len < kMaxTrackLen) ? s_track_len : kMaxTrackLen);
  memset(s_snapshot, 0, sizeof(s_snapshot));
  s_snapshot[0] = mlen;
  memcpy(s_snapshot + 1, s_track, mlen);
  s_snapshot[33] = 0xA5;
  s_snapshot[kSnapTransportOff + 0] = 5;
  s_snapshot[kSnapTransportOff + 1] = static_cast<uint8_t>(s_elapsed_s & 0xffu);
  s_snapshot[kSnapTransportOff + 2] = static_cast<uint8_t>((s_elapsed_s >> 8) & 0xffu);
  s_snapshot[kSnapTransportOff + 3] = static_cast<uint8_t>(s_duration_s & 0xffu);
  s_snapshot[kSnapTransportOff + 4] = static_cast<uint8_t>((s_duration_s >> 8) & 0xffu);
  s_snapshot[kSnapTransportOff + 5] = s_vol_0_15;
}

#if AUDIO_BRIDGE_SERIAL_METADATA
static void serial_log_metadata_line() {
  static uint32_t s_last_meta_serial_ms;
  static size_t s_last_meta_serial_len = static_cast<size_t>(-1);
  const uint32_t now_ms = millis();
  if (s_track_len != s_last_meta_serial_len || (now_ms - s_last_meta_serial_ms) >= 3000u) {
    s_last_meta_serial_ms = now_ms;
    s_last_meta_serial_len = s_track_len;
    Serial.print(F("AVRCP line len="));
    Serial.print(static_cast<unsigned>(s_track_len));
    Serial.print(F(" | "));
    Serial.println(s_track);
  }
}
#endif

void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
  if (!text) {
    return;
  }
  portENTER_CRITICAL(&s_track_mux);
  switch (id) {
    case ESP_AVRC_MD_ATTR_ARTIST:
      strncpy(s_meta_artist, reinterpret_cast<const char *>(text), kMaxTrackLen);
      s_meta_artist[kMaxTrackLen] = '\0';
      break;
    case ESP_AVRC_MD_ATTR_TITLE:
      strncpy(s_meta_title, reinterpret_cast<const char *>(text), kMaxTrackLen);
      s_meta_title[kMaxTrackLen] = '\0';
      break;
    case ESP_AVRC_MD_ATTR_ALBUM:
      // Keep album out of the short OLED line; title/artist are enough for 32 chars.
      break;
    case ESP_AVRC_MD_ATTR_PLAYING_TIME: {
      /* Decimal milliseconds string from phone; drives duration for P4 transport read (reg 0x05). */
      const unsigned long ms = strtoul(reinterpret_cast<const char *>(text), nullptr, 10);
      const unsigned long sec = ms / 1000UL;
      s_duration_s = (sec > 65535UL) ? 65535 : (uint16_t)sec;
      break;
    }
    default:
      break;
  }
  rebuild_track_line();
  if (strncmp(s_track, s_prev_track_for_elapsed, kMaxTrackLen + 1) != 0) {
    strncpy(s_prev_track_for_elapsed, s_track, kMaxTrackLen + 1);
    s_prev_track_for_elapsed[kMaxTrackLen] = '\0';
    anchor_position_unlocked(0);
  }
  rebuild_snapshot_unlocked();
  portEXIT_CRITICAL(&s_track_mux);
}

static void avrc_play_pos_cb(uint32_t play_pos_ms) {
  portENTER_CRITICAL(&s_track_mux);
  s_last_play_pos_wall_ms = millis();
  const uint32_t sec = play_pos_ms / 1000u;
  anchor_position_unlocked((sec > 65535u) ? 65535 : (uint16_t)sec);
  rebuild_snapshot_unlocked();
  portEXIT_CRITICAL(&s_track_mux);
}

static void avrc_play_status_cb(esp_avrc_playback_stat_t st) {
  portENTER_CRITICAL(&s_track_mux);
  apply_elapsed_estimate_unlocked();  // settle elapsed before the state flips
  s_playing = (st == ESP_AVRC_PLAYBACK_PLAYING);
  anchor_position_unlocked(s_elapsed_s);
  rebuild_snapshot_unlocked();
  portEXIT_CRITICAL(&s_track_mux);
}

static void avrc_volume_cb(int vol_0_127) {
  int v = vol_0_127;
  if (v < 0) {
    v = 0;
  }
  if (v > 127) {
    v = 127;
  }
  portENTER_CRITICAL(&s_track_mux);
  s_vol_0_15 = (uint8_t)((v * 15 + 63) / 127);
  rebuild_snapshot_unlocked();
  portEXIT_CRITICAL(&s_track_mux);
}

/** Phone disconnected: clear now-playing so the center doesn't keep showing a stale track. */
static void a2dp_connection_cb(esp_a2d_connection_state_t state, void *) {
  if (state != ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    return;
  }
  portENTER_CRITICAL(&s_track_mux);
  s_meta_artist[0] = '\0';
  s_meta_title[0] = '\0';
  s_prev_track_for_elapsed[0] = '\0';
  rebuild_track_line();
  s_playing = false;
  s_duration_s = 0;
  anchor_position_unlocked(0);
  rebuild_snapshot_unlocked();
  portEXIT_CRITICAL(&s_track_mux);
}

void onReceive(int numBytes) {
  if (numBytes < 1) {
    return;
  }

  const uint8_t reg = Wire.read();
  s_i2c_cmd = reg;

  if (numBytes == 1) {
    return;
  }

  if (reg == kRegSnapshot) {
    while (Wire.available()) {
      (void)Wire.read();
    }
    return;
  }

  const uint8_t count = Wire.read();
  if (reg == kRegTransport) {
    if (count >= 5) {
      uint8_t e0 = 0, e1 = 0, d0 = 0, d1 = 0, vv = 0;
      if (Wire.available()) {
        e0 = static_cast<uint8_t>(Wire.read());
      }
      if (Wire.available()) {
        e1 = static_cast<uint8_t>(Wire.read());
      }
      if (Wire.available()) {
        d0 = static_cast<uint8_t>(Wire.read());
      }
      if (Wire.available()) {
        d1 = static_cast<uint8_t>(Wire.read());
      }
      if (Wire.available()) {
        vv = static_cast<uint8_t>(Wire.read());
      }
      while (Wire.available()) {
        (void)Wire.read();
      }
      portENTER_CRITICAL(&s_track_mux);
      anchor_position_unlocked((uint16_t)((uint16_t)e0 | ((uint16_t)e1 << 8)));
      s_duration_s = (uint16_t)((uint16_t)d0 | ((uint16_t)d1 << 8));
      s_vol_0_15 = vv > 15 ? 15 : vv;
      rebuild_snapshot_unlocked();
      portEXIT_CRITICAL(&s_track_mux);
    } else {
      while (Wire.available()) {
        (void)Wire.read();
      }
    }
    return;
  }

  if (reg != kRegMetadata) {
    while (Wire.available()) {
      (void)Wire.read();
    }
    return;
  }

  /* Multi-byte writes to the metadata register are not part of the P4 protocol (the center only
   * writes the 1-byte register select before a read). This handler used to clear AVRCP
   * artist/title and replace s_track from host bytes — any spurious 2+ byte transaction (noise,
   * tooling, or a bad second byte) wiped AVRCP state so onRequest() always sent len=0. */
  while (Wire.available()) {
    (void)Wire.read();
  }
}

void onRequest() {
  /* Read once: on some ESP32 slave stacks onRequest can race with onReceive updating s_i2c_cmd. */
  const uint8_t cmd = s_i2c_cmd;
  uint8_t out[kSnapshotLen];
  portENTER_CRITICAL(&s_track_mux);
  memcpy(out, s_snapshot, sizeof(out));
  portEXIT_CRITICAL(&s_track_mux);

  if (cmd == kRegSnapshot) {
    Wire.write(out, kSnapshotLen);
  } else if (cmd == kRegMetadata) {
    Wire.write(out, 1 + kMaxTrackLen);  // [len][32 bytes, zero padded] — hosts clock 33 bytes
  } else if (cmd == kRegTransport) {
    Wire.write(out + kSnapTransportOff, kTransportLen);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("esp32_audio_bridge: A2DP sink + AVRCP + I2C"));
#if AUDIO_BRIDGE_USE_INTERNAL_DAC
  Serial.println(F("Audio: internal DAC (GPIO25 L / GPIO26 R)"));
#else
  Serial.println(F("Audio: I2S → external DAC"));
#endif

  memset(s_track, 0, sizeof(s_track));
  memset(s_meta_artist, 0, sizeof(s_meta_artist));
  memset(s_meta_title, 0, sizeof(s_meta_title));
  memset(s_prev_track_for_elapsed, 0, sizeof(s_prev_track_for_elapsed));
  s_track_len = 0;
  s_elapsed_s = 0;
  s_duration_s = 0;
  s_vol_0_15 = 0;
  s_last_play_pos_wall_ms = 0;
  s_playing = false;
  anchor_position_unlocked(0);
  rebuild_snapshot_unlocked();

  {
#if AUDIO_BRIDGE_USE_INTERNAL_DAC
    auto cfg = audio_output.defaultConfig(TX_MODE);
    cfg.sample_rate = 44100;
    cfg.channels = 2;
    cfg.bits_per_sample = 16;
    if (!audio_output.begin(cfg)) {
      Serial.println(F("audio_output.begin (internal DAC) failed"));
    }
#else
    auto cfg = audio_output.defaultConfig(TX_MODE);
    cfg.pin_bck = I2S_PIN_BCK;
    cfg.pin_ws = I2S_PIN_WS;
    cfg.pin_data = I2S_PIN_DATA;
    if (!audio_output.begin(cfg)) {
      Serial.println(F("audio_output.begin (I2S) failed"));
    }
#endif
  }

  /* I2C slave before A2DP/BT: host may poll 0x2A as soon as wires are live; avoid missing early reads. */
  Wire.begin(static_cast<uint8_t>(I2C_SLAVE_ADDR), I2C_SDA_PIN, I2C_SCL_PIN, 100000U);
  Wire.onReceive(onReceive);
  Wire.onRequest(onRequest);

#if CLUSTER_THEME_I2C_ENABLE
  Wire1.begin(static_cast<uint8_t>(CLUSTER_THEME_I2C_ADDR), CLUSTER_THEME_SDA, CLUSTER_THEME_SCL, 100000U);
  Wire1.onReceive(on_cluster_theme_receive);
#endif

  a2dp_sink.set_avrc_metadata_attribute_mask(
      ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM |
      ESP_AVRC_MD_ATTR_TRACK_NUM | ESP_AVRC_MD_ATTR_NUM_TRACKS | ESP_AVRC_MD_ATTR_GENRE |
      ESP_AVRC_MD_ATTR_PLAYING_TIME);
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_avrc_rn_playstatus_callback(avrc_play_status_cb);
  /* Default in library was 10s between position notifications — too coarse for a dash. */
  a2dp_sink.set_avrc_rn_play_pos_callback(avrc_play_pos_cb, 1);
  a2dp_sink.set_avrc_rn_volumechange(avrc_volume_cb);
  a2dp_sink.set_on_connection_state_changed(a2dp_connection_cb);
  a2dp_sink.set_auto_reconnect(true);
  a2dp_sink.start(BT_DEVICE_NAME);

  Serial.print(F("BT name: "));
  Serial.println(BT_DEVICE_NAME);
  Serial.print(F("I2C metadata 0x"));
  Serial.println(I2C_SLAVE_ADDR, HEX);
#if CLUSTER_THEME_I2C_ENABLE
  Serial.print(F("I2C cluster theme 0x"));
  Serial.print(CLUSTER_THEME_I2C_ADDR, HEX);
  Serial.print(F(" SDA="));
  Serial.print(CLUSTER_THEME_SDA);
  Serial.print(F(" SCL="));
  Serial.println(CLUSTER_THEME_SCL);
#endif
}

void loop() {
  // Keep the elapsed estimate in the reply buffer moving between AVRCP events.
  portENTER_CRITICAL(&s_track_mux);
  rebuild_snapshot_unlocked();
  portEXIT_CRITICAL(&s_track_mux);
#if AUDIO_BRIDGE_SERIAL_METADATA
  serial_log_metadata_line();
#endif
  delay(250);
}
