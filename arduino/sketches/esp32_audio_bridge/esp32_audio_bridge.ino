/*
 * esp32_audio_bridge.ino — in-dash Bluetooth media hub (ESP32 classic BT only)
 *
 * - A2DP sink: phone / portable player streams stereo audio to I2S (vehicle DAC / amp).
 * - AVRCP 1.x: title, artist, album, track #, genre, duration (typical car-infotainment metadata).
 * - I2C slave for the Raspberry Pi (Esp32AudioLink): Pi can push a short line (local media),
 *   or read back the current line (SMBus-style block read on register 0x01).
 *
 * HARDWARE: Bluetooth Classic (A2DP) exists on the original ESP32 only — not ESP32-S3 / C3 / S2.
 * Use a module such as ESP32-WROOM-32 and set FQBN to esp32:esp32:esp32 when building.
 *
 * Default I2C address 0x2A matches config integrations.esp32_audio_link.address.
 * I2S defaults match ESP32-A2DP + AudioTools (wire BCK/WS/DOUT to your PCM DAC).
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

// I2S to external DAC (override per your PCB)
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
static constexpr size_t kMaxTrackLen = 32;

static char s_track[kMaxTrackLen + 1];
static volatile size_t s_track_len;
static volatile uint8_t s_i2c_cmd = kRegMetadata;

static char s_meta_artist[kMaxTrackLen + 1];
static char s_meta_title[kMaxTrackLen + 1];
static portMUX_TYPE s_track_mux = portMUX_INITIALIZER_UNLOCKED;

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

I2SStream i2s;
BluetoothA2DPSink a2dp_sink(i2s);

static void rebuild_track_line() {
  s_meta_artist[kMaxTrackLen] = '\0';
  s_meta_title[kMaxTrackLen] = '\0';

  const char *a = s_meta_artist[0] ? s_meta_artist : "";
  const char *t = s_meta_title[0] ? s_meta_title : "";
  if (a[0] && t[0]) {
    snprintf(s_track, sizeof(s_track), "%s - %s", a, t);
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
    default:
      break;
  }
  rebuild_track_line();
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

  const uint8_t count = Wire.read();
  if (reg != kRegMetadata) {
    while (Wire.available()) {
      (void)Wire.read();
    }
    return;
  }

  portENTER_CRITICAL(&s_track_mux);
  s_meta_artist[0] = '\0';
  s_meta_title[0] = '\0';

  const int toRead = min(static_cast<int>(count), Wire.available());
  size_t n = 0;
  for (int i = 0; i < toRead && n < kMaxTrackLen; ++i) {
    const int c = Wire.read();
    if (c < 0) {
      break;
    }
    s_track[n++] = static_cast<char>(c);
  }
  while (Wire.available()) {
    (void)Wire.read();
  }
  s_track[n] = '\0';
  s_track_len = n;
  portEXIT_CRITICAL(&s_track_mux);
}

void onRequest() {
  if (s_i2c_cmd != kRegMetadata) {
    return;
  }
  portENTER_CRITICAL(&s_track_mux);
  const size_t n = s_track_len;
  const uint8_t len = static_cast<uint8_t>(min(n, kMaxTrackLen));
  Wire.write(len);
  for (uint8_t i = 0; i < len; ++i) {
    Wire.write(static_cast<uint8_t>(s_track[i]));
  }
  portEXIT_CRITICAL(&s_track_mux);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("esp32_audio_bridge: A2DP sink + AVRCP + I2C"));

  memset(s_track, 0, sizeof(s_track));
  memset(s_meta_artist, 0, sizeof(s_meta_artist));
  memset(s_meta_title, 0, sizeof(s_meta_title));
  s_track_len = 0;

  {
    auto cfg = i2s.defaultConfig();
    cfg.pin_bck = I2S_PIN_BCK;
    cfg.pin_ws = I2S_PIN_WS;
    cfg.pin_data = I2S_PIN_DATA;
    i2s.begin(cfg);
  }

  a2dp_sink.set_avrc_metadata_attribute_mask(
      ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM |
      ESP_AVRC_MD_ATTR_TRACK_NUM | ESP_AVRC_MD_ATTR_NUM_TRACKS | ESP_AVRC_MD_ATTR_GENRE |
      ESP_AVRC_MD_ATTR_PLAYING_TIME);
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_auto_reconnect(true);
  a2dp_sink.start(BT_DEVICE_NAME);

  Wire.begin(static_cast<uint8_t>(I2C_SLAVE_ADDR), I2C_SDA_PIN, I2C_SCL_PIN, 100000U);
  Wire.onReceive(onReceive);
  Wire.onRequest(onRequest);

#if CLUSTER_THEME_I2C_ENABLE
  s_cluster_prefs.begin(CLUSTER_NVS_NAMESPACE, false);
  Wire1.begin(static_cast<uint8_t>(CLUSTER_THEME_I2C_ADDR), CLUSTER_THEME_SDA, CLUSTER_THEME_SCL, 100000U);
  Wire1.onReceive(on_cluster_theme_receive);
#endif

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
  delay(500);
}
