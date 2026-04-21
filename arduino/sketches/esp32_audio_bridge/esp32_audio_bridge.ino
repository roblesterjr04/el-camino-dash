/*
 * esp32_audio_bridge.ino — in-dash Bluetooth media hub (ESP32 classic BT only)
 *
 * - A2DP sink: phone / portable player streams stereo audio to the ESP32 **internal** DAC
 *   (GPIO25 / GPIO26 — classic ESP32 only) by default; optional I2S → external DAC/amp via define.
 * - AVRCP 1.x: title, artist, album, track #, genre, duration (typical car-infotainment metadata).
 * - I2C slave for the Raspberry Pi (Esp32AudioLink): Pi can push a short line (local media),
 *   or read back the current line (SMBus-style block read on register 0x01).
 *
 * HARDWARE: Bluetooth Classic (A2DP) exists on the original ESP32 only — not ESP32-S3 / C3 / S2.
 * Use a module such as ESP32-WROOM-32 and set FQBN to esp32:esp32:esp32 when building.
 *
 * Default I2C address 0x2A matches config integrations.esp32_audio_link.address.
 * External I2S: wire BCK/WS/DOUT per I2S_PIN_* and build with AUDIO_BRIDGE_USE_INTERNAL_DAC=0
 * (see firmware/esp32_classic_bt_audio/platformio.ini env esp32dev_external_i2s).
 *
 * ESP32-P4 center (Waveshare): connect this chip's I2C to the **same bus** as the round
 * display — **SDA → P4 GPIO 7**, **SCL → P4 GPIO 8** (default Arduino ESP32: SDA=GPIO21,
 * SCL=GPIO22). Re-map I2S or I2C in #defines if GPIO 22 is used twice below.
 */

#include <Arduino.h>
#include <Preferences.h>
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

// I2S to external DAC (override per your PCB; ignored when AUDIO_BRIDGE_USE_INTERNAL_DAC=1)
#ifndef I2S_PIN_BCK
#define I2S_PIN_BCK 14
#endif
#ifndef I2S_PIN_WS
#define I2S_PIN_WS 15
#endif
#ifndef I2S_PIN_DATA
#define I2S_PIN_DATA 22
#endif

static constexpr uint8_t kRegMetadata = 0x01;
static constexpr uint8_t kRegTransport = 0x05;
/** P4 center: one atomic read for title + transport (avoids s_i2c_cmd / onRequest races). Pi keeps using 0x01/0x05. */
static constexpr uint8_t kRegSnapshot = 0x0F;
static constexpr size_t kMaxTrackLen = 32;

static char s_track[kMaxTrackLen + 1];
static volatile size_t s_track_len;
static volatile uint8_t s_i2c_cmd = kRegMetadata;

static uint16_t s_elapsed_s;
static uint16_t s_duration_s;
static uint8_t s_vol_0_15;

static char s_meta_artist[kMaxTrackLen + 1];
static char s_meta_title[kMaxTrackLen + 1];
static portMUX_TYPE s_track_mux = portMUX_INITIALIZER_UNLOCKED;

/* Elapsed when the phone does not send play-position notifications (common) or only rarely. */
static uint32_t s_track_start_ms;
static uint32_t s_last_play_pos_wall_ms;
static bool s_playing;
static char s_prev_track_for_elapsed[kMaxTrackLen + 1];

static void apply_elapsed_estimate_unlocked() {
  const uint32_t now = millis();
  if (!s_playing || s_duration_s == 0) {
    return;
  }
  if (now - s_last_play_pos_wall_ms < 2000u) {
    return;
  }
  uint32_t est = (now - s_track_start_ms) / 1000u;
  if (est > (uint32_t)s_duration_s) {
    est = s_duration_s;
  }
  s_elapsed_s = (est > 65535u) ? 65535 : (uint16_t)est;
}

#if CLUSTER_THEME_I2C_ENABLE
static Preferences s_cluster_prefs;

static void on_cluster_theme_receive(int num_bytes) {
  if (num_bytes < 2) {
    while (Wire1.available()) {
      (void)Wire1.read();
    }
    return;
  }
  const uint8_t reg = Wire1.read();
  const uint8_t val = Wire1.read();
  while (Wire1.available()) {
    (void)Wire1.read();
  }
  if (reg == CLUSTER_REG_THEME) {
    s_cluster_prefs.putUChar(CLUSTER_NVS_KEY_THEME, val);
  } else if (reg == CLUSTER_REG_CENTER_SUBVIEW) {
    // Optional: store if you later want the ESP32-D to react to center sub-view.
    (void)val;
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
    s_track_start_ms = millis();
  }
  portEXIT_CRITICAL(&s_track_mux);
#if AUDIO_BRIDGE_SERIAL_METADATA
  serial_log_metadata_line();
#endif
}

static void avrc_play_pos_cb(uint32_t play_pos_ms) {
  portENTER_CRITICAL(&s_track_mux);
  s_last_play_pos_wall_ms = millis();
  const uint32_t sec = play_pos_ms / 1000u;
  s_elapsed_s = (sec > 65535u) ? 65535 : (uint16_t)sec;
  portEXIT_CRITICAL(&s_track_mux);
}

static void avrc_play_status_cb(esp_avrc_playback_stat_t st) {
  portENTER_CRITICAL(&s_track_mux);
  s_playing = (st == ESP_AVRC_PLAYBACK_PLAYING);
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
      s_elapsed_s = (uint16_t)((uint16_t)e0 | ((uint16_t)e1 << 8));
      s_duration_s = (uint16_t)((uint16_t)d0 | ((uint16_t)d1 << 8));
      s_vol_0_15 = vv > 15 ? 15 : vv;
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

  if (cmd == kRegSnapshot) {
    portENTER_CRITICAL(&s_track_mux);
    apply_elapsed_estimate_unlocked();
    rebuild_track_line();
    const size_t tn = s_track_len;
    const uint8_t mlen = static_cast<uint8_t>((tn < kMaxTrackLen) ? tn : kMaxTrackLen);
    Wire.write(mlen);
    for (uint8_t i = 0; i < 32; i++) {
      Wire.write(i < mlen ? static_cast<uint8_t>(s_track[i]) : 0);
    }
    Wire.write(0xA5);
    Wire.write(5);
    Wire.write(static_cast<uint8_t>(s_elapsed_s & 0xffu));
    Wire.write(static_cast<uint8_t>((s_elapsed_s >> 8) & 0xffu));
    Wire.write(static_cast<uint8_t>(s_duration_s & 0xffu));
    Wire.write(static_cast<uint8_t>((s_duration_s >> 8) & 0xffu));
    Wire.write(s_vol_0_15);
    for (int p = 0; p < 8; p++) {
      Wire.write(0);
    }
    portEXIT_CRITICAL(&s_track_mux);
    return;
  }

  if (cmd == kRegMetadata) {
    portENTER_CRITICAL(&s_track_mux);
    apply_elapsed_estimate_unlocked();
    /* Recompute from s_meta_* so I2C never serves a stale/empty s_track if AVRCP updated it. */
    rebuild_track_line();
    const size_t n = s_track_len;
    const uint8_t len = static_cast<uint8_t>(min(n, kMaxTrackLen));
    Wire.write(len);
    for (uint8_t i = 0; i < len; ++i) {
      Wire.write(static_cast<uint8_t>(s_track[i]));
    }
    /* Center (P4) clocks 33 bytes per read; pad so the slave response length matches the master
     * request — short slave replies caused flaky metadata on some stacks. */
    for (int pad = 1 + (int)len; pad < 33; pad++) {
      Wire.write(0);
    }
    portEXIT_CRITICAL(&s_track_mux);
    return;
  }
  if (cmd == kRegTransport) {
    portENTER_CRITICAL(&s_track_mux);
    apply_elapsed_estimate_unlocked();
    Wire.write(5);
    Wire.write(static_cast<uint8_t>(s_elapsed_s & 0xffu));
    Wire.write(static_cast<uint8_t>((s_elapsed_s >> 8) & 0xffu));
    Wire.write(static_cast<uint8_t>(s_duration_s & 0xffu));
    Wire.write(static_cast<uint8_t>((s_duration_s >> 8) & 0xffu));
    Wire.write(s_vol_0_15);
    portEXIT_CRITICAL(&s_track_mux);
    return;
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
  s_track_start_ms = millis();
  s_last_play_pos_wall_ms = 0;
  s_playing = false;

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
  s_cluster_prefs.begin(CLUSTER_NVS_NAMESPACE, false);
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
  delay(10);
}
