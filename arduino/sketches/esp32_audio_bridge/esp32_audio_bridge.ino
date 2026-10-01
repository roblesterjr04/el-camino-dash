/*
 * esp32_audio_bridge.ino — in-dash Bluetooth media hub (ESP32 classic BT only)
 *
 * - A2DP sink: phone / portable player streams stereo audio over I2S to an external DAC/amp
 *   (BCK/WS/DOUT per I2S_PIN_*).
 * - AVRCP 1.x: title, artist, album, track #, genre, duration (typical car-infotainment metadata).
 * - HFP hands-free: calls through the same DACs, microphone from the carrier's ADC (call_audio.*); pairing with
 *   numeric comparison and the paired-phone list (phone.*). The console drives it over CAN (0x215); the serial
 *   console has the same commands for the bench (`help`).
 * - I2C slave @ 0x2A for the center ESP32-P4: it reads a 48-byte snapshot (register 0x0F) with the
 *   "now playing" line + elapsed/duration/volume. Registers 0x01 / 0x05 remain for older hosts.
 *
 * HARDWARE: Bluetooth Classic (A2DP) exists on the original ESP32 only — not ESP32-S3 / C3 / S2.
 * Use a module such as ESP32-WROOM-32 and set FQBN to esp32:esp32:esp32 when building.
 *
 * ESP32-P4 center (Waveshare): connect this chip's I2C to the **same bus** as the round
 * display — **SDA → P4 GPIO 7**, **SCL → P4 GPIO 8** (this chip: SDA=GPIO21, SCL=GPIO22).
 *
 * CAN accessory (an audio carrier on a DigiDash accessory port, docs/hardware/accessory_ports.md; or slot 0 on
 * the first board revision): when built with the elc_can library (PlatformIO, firmware/esp32_classic_bt_audio),
 * the bridge joins the CAN bus through the carrier's MCP2562 (TX = GPIO32, RX = GPIO33). Its node number: on the
 * rev 2 audio card (ELC_CAN_CARD_SLOT_PIN set) from the slot it sits in, 0x14 + slot - 1, read from the slot ID pin;
 * on a rev 1 carrier from the GPIO13 / GPIO23 straps (grounded = 1; both open = node 0x10). It sends a heartbeat and
 * the now-playing text + transport in its node's ID range, and serves album art on request
 * (docs/protocols/can_bus.md). CAN is the link in the car: the I2C slaves (0x2A metadata, 0x3E
 * theme) are for bench testing only and are started only when AUDIO_BRIDGE_I2C=1 (PlatformIO env
 * esp32dev_bench, or the plain arduino-cli build, which has no CAN).
 *
 * Threading: AVRCP callbacks (BT task) update state and rebuild a prebuilt reply buffer under a
 * spinlock; the I2C onRequest handler only copies that buffer, so it answers quickly.
 */

#include <Arduino.h>
#include <Wire.h>
#include <cstring>

#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include "art_variants.h"
#include "call_audio.h"
#include "cluster_theme_i2c.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "phone.h"

#include <cstdlib>

// CAN is built in when the elc_can library is on the include path (PlatformIO build). The plain
// arduino-cli build (arduino/scripts) has no CAN and behaves as before.
#if __has_include(<elc_can.h>)
#include <elc_can.h>
#define AUDIO_BRIDGE_CAN 1
#else
#define AUDIO_BRIDGE_CAN 0
#endif
// Carrier pinout: CAN TX/RX to the carrier's transceiver, node straps (grounded = 1; open = 0 = node 0x10).
#ifndef ELC_CAN_TX_PIN
#define ELC_CAN_TX_PIN 32
#endif
#ifndef ELC_CAN_RX_PIN
#define ELC_CAN_RX_PIN 33
#endif
#ifndef ELC_CAN_SLOT_ID0_PIN
#define ELC_CAN_SLOT_ID0_PIN 13
#endif
#ifndef ELC_CAN_SLOT_ID1_PIN
#define ELC_CAN_SLOT_ID1_PIN 23
#endif
// Rev 2 slot card: the slot ID pin (slot pin 11, 10k to 3.3 V on the card) on this ADC pin; -1 = a rev 1 carrier.
#ifndef ELC_CAN_CARD_SLOT_PIN
#define ELC_CAN_CARD_SLOT_PIN -1
#endif
// Bench only: the node to use when the card reads no slot (no main board). Unset: CAN stays off without a slot.
#ifndef ELC_CAN_BENCH_NODE
#define ELC_CAN_BENCH_NODE 0
#endif

/* Set to 1 (or -D in platformio.ini) to print AVRCP-built line on Serial when it changes / every 3s. */
#ifndef AUDIO_BRIDGE_SERIAL_METADATA
#define AUDIO_BRIDGE_SERIAL_METADATA 0
#endif

#ifndef BT_DEVICE_NAME
#define BT_DEVICE_NAME "ElCamino-Media"
#endif

// I2C slaves (metadata 0x2A + theme 0x3E): bench only. Default on when CAN is not built in.
#ifndef AUDIO_BRIDGE_I2C
#define AUDIO_BRIDGE_I2C (!AUDIO_BRIDGE_CAN)
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
#define CLUSTER_THEME_I2C_ENABLE AUDIO_BRIDGE_I2C
#endif
#ifndef CLUSTER_THEME_SDA
#define CLUSTER_THEME_SDA 18
#endif
#ifndef CLUSTER_THEME_SCL
#define CLUSTER_THEME_SCL 19
#endif

// I2S to the external DAC/amp (override per your PCB).
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
// Call audio (HFP): microphone ADC data in and its master clock (the audio carrier: PCM1808 on GPIO35, MCLK on GPIO0),
// and the DACs' soft-mute line (carrier: GPIO4, high = play). -1 = not fitted (a DevKit with a DAC only: calls play,
// the phone hears silence).
#ifndef I2S_PIN_DIN
#define I2S_PIN_DIN -1
#endif
#ifndef I2S_PIN_MCLK
#define I2S_PIN_MCLK -1
#endif
#ifndef DAC_UNMUTE_PIN
#define DAC_UNMUTE_PIN -1
#endif
#ifndef MIC_RIGHT_CHANNEL
#define MIC_RIGHT_CHANNEL 0
#endif
#if AUDIO_BRIDGE_CAN && (I2S_PIN_DATA == ELC_CAN_TX_PIN || I2S_PIN_WS == ELC_CAN_TX_PIN || I2S_PIN_BCK == ELC_CAN_TX_PIN || \
                         I2S_PIN_DATA == ELC_CAN_RX_PIN || I2S_PIN_WS == ELC_CAN_RX_PIN || I2S_PIN_BCK == ELC_CAN_RX_PIN)
#error "I2S pins collide with the CAN transceiver pins of the DigiDash slot"
#endif

static constexpr uint8_t kRegMetadata = 0x01;
static constexpr uint8_t kRegTransport = 0x05;
/** P4 center: one atomic read for title + transport (avoids s_i2c_cmd / onRequest races). Pi keeps using 0x01/0x05. */
static constexpr uint8_t kRegSnapshot = 0x0F;
static constexpr size_t kMaxTrackLen = 32;
/** Snapshot layout: [mlen][meta:32][0xA5][5][e0][e1][d0][d1][vol][0xC7][art_id][len0][len1][art_state][pad:3]
 *  = 48 bytes. The metadata reply (0x01) is bytes 0..32 and the transport reply (0x05) is bytes 34..39
 *  of the same buffer; bytes 40..44 describe the album art (hosts that predate it ignore them). */
static constexpr size_t kSnapshotLen = 48;
static constexpr size_t kSnapTransportOff = 34;
static constexpr size_t kTransportLen = 6;
static constexpr size_t kSnapArtOff = 40;

/* Album art (AVRCP 1.6 cover art, the phone's 200×200 JPEG thumbnail). The host reads it in chunks:
 * write [0x10][idx_lo][idx_hi], then read [0xC7][art_id][idx_lo][idx_hi][n][n data bytes] (kArtChunk
 * max). art_id is 0 while no complete image is available; the host checks the header of each chunk. */
static constexpr uint8_t kRegArtChunk = 0x10;
static constexpr uint8_t kArtMagic = 0xC7;
static constexpr size_t kArtChunk = 120;
#if defined(BOARD_HAS_PSRAM)
static constexpr size_t kArtMax = 36 * 1024;
#else
// Without PSRAM the buffer comes out of the heap Bluetooth needs (A2DP + hands-free leave ~30 KB): larger covers
// are skipped (s_art_overflow) rather than starving the stack.
static constexpr size_t kArtMax = 16 * 1024;
#endif
enum : uint8_t { kArtNone = 0, kArtLoading = 1, kArtReady = 2 };
static uint8_t *s_art = nullptr;          // JPEG bytes
static volatile size_t s_art_rx = 0;      // bytes received so far / final size when ready
static volatile uint8_t s_art_state = kArtNone;
static volatile uint8_t s_art_id = 0;     // bumps for every completed image (never 0 when ready)
static volatile bool s_art_overflow = false;
static volatile bool s_ca_supported = false;  // phone offers cover art
static volatile bool s_ca_connected = false;
static bool s_ca_connecting = false;
static uint32_t s_ca_connect_ms = 0;
static uint32_t s_art_req_ms = 0;
static char s_art_want[8];                // handle from the latest metadata ("" = none)
static char s_art_have[8];                // handle requested / shown
static volatile uint16_t s_art_chunk_req = 0;
static constexpr uint8_t kMetadataTl = 7;  // AVRCP transaction label for our own metadata request

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

/* The I2S stream and the sink are created in setup(), not as globals: their constructors allocate
 * through audio-tools' global allocator, which lives in another file and may not be constructed yet
 * (static init order) — as globals they crashed at boot with LoadProhibited in Allocator::allocate. */
static I2SStream *audio_output = nullptr;
static CallGateStream *a2dp_output = nullptr;  // A2DP's way to audio_output, closed while a call owns I2S0
static BluetoothA2DPSink *a2dp_sink = nullptr;

/** Cover art data arrives in pieces on the BT task; copy it out before the callback returns. */
static void on_cover_art_data(const esp_avrc_ct_cb_param_t *p) {
  if (p->cover_art_data.status != ESP_BT_STATUS_SUCCESS) {
    s_art_state = kArtNone;
    return;
  }
  const size_t n = p->cover_art_data.data_len;
  if (s_art && !s_art_overflow && n && p->cover_art_data.p_data) {
    if (s_art_rx + n <= kArtMax) {
      memcpy(s_art + s_art_rx, p->cover_art_data.p_data, n);
      s_art_rx += n;
    } else {
      s_art_overflow = true;
    }
  }
  if (p->cover_art_data.final) {
    if (!s_art_overflow && s_art_rx > 0) {
      s_art_id = static_cast<uint8_t>(s_art_id == 255 ? 1 : s_art_id + 1);
      s_art_state = kArtReady;
    } else {
      s_art_state = kArtNone;
    }
  }
}

/** The library's sink plus AVRCP cover art (it does not handle the cover-art events itself), and the hooks the phone
 *  code needs: pairing events and which phone the media link is with. */
class CoverArtSink : public BluetoothA2DPSink {
 public:
  explicit CoverArtSink(audio_tools::AudioStream &out) : BluetoothA2DPSink(out) {}

 protected:
  void app_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) override {
    BluetoothA2DPSink::app_gap_callback(event, param);
    phone_gap_event(event, param);
  }

  void app_a2d_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) override {
    if (event == ESP_A2D_CONNECTION_STATE_EVT) {
      phone_a2dp_connection(param->conn_stat.state, param->conn_stat.remote_bda);
    }
    BluetoothA2DPSink::app_a2d_callback(event, param);
  }

  void app_rc_ct_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) override {
    switch (event) {
      case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        s_ca_supported = (param->rmt_feats.tg_feat_flag & ESP_AVRC_FEAT_FLAG_TG_COVER_ART) != 0;
        Serial.printf("AVRCP remote features 0x%lx, TG flags 0x%x: cover art %s\n", (unsigned long)param->rmt_feats.feat_mask,
                      (unsigned)param->rmt_feats.tg_feat_flag, s_ca_supported ? "offered" : "not offered");
        break;  // the library handles the rest
      case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (!param->conn_stat.connected) {
          s_ca_supported = false;
          s_ca_connected = false;
        }
        break;
      case ESP_AVRC_CT_COVER_ART_STATE_EVT:
        s_ca_connected = param->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED;
        Serial.printf("cover art channel %s (reason %d)\n", s_ca_connected ? "connected" : "disconnected",
                      (int)param->cover_art_state.reason);
        return;
      case ESP_AVRC_CT_COVER_ART_DATA_EVT:
        on_cover_art_data(param);
        return;
      default:
        break;
    }
    BluetoothA2DPSink::app_rc_ct_callback(event, param);
  }
};

/** Loop: open the cover-art channel when the phone offers it, and fetch art for new handles. */
static void cover_art_poll() {
  const uint32_t now = millis();
  if (!s_ca_supported) {
    s_ca_connecting = false;
    s_art_have[0] = '\0';
    return;
  }
  if (!s_ca_connected) {
    if (!s_ca_connecting || now - s_ca_connect_ms > 5000u) {
      s_ca_connecting = esp_avrc_ct_cover_art_connect(ESP_AVRC_CA_MTU_MAX) == ESP_OK;
      s_ca_connect_ms = now;
    }
    s_art_have[0] = '\0';  // re-request once connected
    return;
  }
  if (s_ca_connecting) {
    // Phones (iOS in particular) only put the cover-art handle in track metadata once this channel
    // is open, and the library asked for metadata at connect time, before it was: ask again.
    esp_avrc_ct_send_metadata_cmd(kMetadataTl, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                                                   ESP_AVRC_MD_ATTR_PLAYING_TIME | ESP_AVRC_MD_ATTR_COVER_ART);
  }
  s_ca_connecting = false;
  static uint8_t s_logged_id = 0;
  if (s_art_state == kArtReady && s_art_id != s_logged_id) {
    s_logged_id = s_art_id;
    Serial.printf("cover art #%u: %u bytes (%s)\n", (unsigned)s_art_id, (unsigned)s_art_rx, s_art_have);
  }
  if (s_art_state == kArtLoading && now - s_art_req_ms < 10000u) {
    return;  // transfer in progress
  }
  char want[8];
  portENTER_CRITICAL(&s_track_mux);
  memcpy(want, s_art_want, sizeof(want));
  portEXIT_CRITICAL(&s_track_mux);
  if (strcmp(want, s_art_have) == 0 && s_art_state != kArtLoading) {
    return;
  }
  memcpy(s_art_have, want, sizeof(want));
  if (!want[0]) {
    s_art_state = kArtNone;  // this track has no art
    return;
  }
  s_art_rx = 0;
  s_art_overflow = false;
  s_art_state = kArtLoading;
  s_art_req_ms = now;
  const esp_err_t err = esp_avrc_ct_cover_art_get_linked_thumbnail(reinterpret_cast<uint8_t *>(s_art_have));
  Serial.printf("cover art: requesting thumbnail %s (%s)\n", s_art_have, esp_err_to_name(err));
  if (err != ESP_OK) {
    s_art_state = kArtNone;
  }
}

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
  const bool ready = s_art_state == kArtReady;
  s_snapshot[kSnapArtOff + 0] = kArtMagic;
  s_snapshot[kSnapArtOff + 1] = ready ? s_art_id : 0;
  s_snapshot[kSnapArtOff + 2] = static_cast<uint8_t>(ready ? (s_art_rx & 0xffu) : 0);
  s_snapshot[kSnapArtOff + 3] = static_cast<uint8_t>(ready ? ((s_art_rx >> 8) & 0xffu) : 0);
  s_snapshot[kSnapArtOff + 4] = s_art_state;
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
    case ESP_AVRC_MD_ATTR_COVER_ART:
      strncpy(s_art_want, reinterpret_cast<const char *>(text), sizeof(s_art_want) - 1);
      s_art_want[sizeof(s_art_want) - 1] = '\0';
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
    if (id != ESP_AVRC_MD_ATTR_COVER_ART) {
      s_art_want[0] = '\0';  // new track: its cover art handle (if any) follows in this response
    }
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

static void on_receive_impl(int numBytes) {
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

  if (reg == kRegArtChunk) {
    const int lo = Wire.read();
    const int hi = Wire.available() ? Wire.read() : 0;
    s_art_chunk_req = static_cast<uint16_t>((lo & 0xff) | ((hi & 0xff) << 8));
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

static void preload_reply(uint8_t reg);

void onReceive(int numBytes) {
  on_receive_impl(numBytes);
  preload_reply(s_i2c_cmd);
}

/** Build the reply for register `reg` and load it into the slave TX FIFO right away. On the classic
 *  ESP32 the Arduino slave only calls onRequest() *after* a read has finished, so whatever it writes
 *  goes out on the next read (every reply one behind). Preloading from onReceive(), when the register
 *  select arrives, makes each read return its own reply; the host waits ~1 ms before reading. */
static void preload_reply(uint8_t reg) {
  uint8_t out[kSnapshotLen];
  portENTER_CRITICAL(&s_track_mux);
  memcpy(out, s_snapshot, sizeof(out));
  portEXIT_CRITICAL(&s_track_mux);
  if (reg == kRegSnapshot) {
    Wire.slaveWrite(out, kSnapshotLen);
  } else if (reg == kRegMetadata) {
    Wire.slaveWrite(out, 1 + kMaxTrackLen);  // [len][32 bytes, zero padded] — hosts clock 33 bytes
  } else if (reg == kRegTransport) {
    Wire.slaveWrite(out + kSnapTransportOff, kTransportLen);
  } else if (reg == kRegArtChunk) {
    uint8_t buf[5 + kArtChunk];
    const uint16_t idx = s_art_chunk_req;
    const bool ready = s_art_state == kArtReady && s_art;
    const size_t off = (size_t)idx * kArtChunk;
    const size_t n = (ready && off < s_art_rx) ? ((s_art_rx - off) < kArtChunk ? (s_art_rx - off) : kArtChunk) : 0;
    buf[0] = kArtMagic;
    buf[1] = ready ? s_art_id : 0;
    buf[2] = static_cast<uint8_t>(idx & 0xffu);
    buf[3] = static_cast<uint8_t>(idx >> 8);
    buf[4] = static_cast<uint8_t>(n);
    memset(buf + 5, 0, kArtChunk);
    if (n) {
      memcpy(buf + 5, s_art + off, n);
    }
    Wire.slaveWrite(buf, sizeof(buf));
  }
}

/** Replies are preloaded in onReceive(); see preload_reply(). */
void onRequest() {}

/** A phone command from CAN or the serial console. Media transport and volume are the sink's; the rest is phone.cpp. */
static bool run_phone_command(uint8_t cmd, uint8_t arg, uint8_t list_serial, bool check_serial) {
  if (cmd == kPhoneCmdMedia) {
    if (!a2dp_sink || !a2dp_sink->is_connected()) return false;
    if (arg == 1) a2dp_sink->next();
    else if (arg == 2) a2dp_sink->previous();
    else if (a2dp_sink->get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED) a2dp_sink->pause();
    else a2dp_sink->play();
    return true;
  }
  if (cmd == kPhoneCmdMediaVolume) {
    if (!a2dp_sink) return false;
    const uint8_t v = arg > 15 ? 15 : arg;
    a2dp_sink->set_volume((uint8_t)((v * 127 + 7) / 15));  // AVRCP absolute volume: the phone's slider follows
    portENTER_CRITICAL(&s_track_mux);
    s_vol_0_15 = v;
    rebuild_snapshot_unlocked();
    portEXIT_CRITICAL(&s_track_mux);
    return true;
  }
  return phone_command(cmd, arg, list_serial, check_serial);
}

#if AUDIO_BRIDGE_CAN
// phone.h mirrors the CAN numbering so it builds without the CAN library; keep them in step.
static_assert(kPhoneConnected == (int)ELC_CAN_PHONE_CONNECTED && kPhoneHfp == (int)ELC_CAN_PHONE_HFP &&
                  kPhoneCallAudio == (int)ELC_CAN_PHONE_CALL_AUDIO && kPhoneMicMuted == (int)ELC_CAN_PHONE_MIC_MUTED &&
                  kPhonePairing == (int)ELC_CAN_PHONE_PAIRING && kPhoneConfirm == (int)ELC_CAN_PHONE_CONFIRM &&
                  kPhoneInbandRing == (int)ELC_CAN_PHONE_INBAND_RING && kPhoneVoice == (int)ELC_CAN_PHONE_VOICE,
              "phone flags");
static_assert(kCallIncoming == (int)ELC_CAN_CALL_INCOMING && kCallDialing == (int)ELC_CAN_CALL_DIALING &&
                  kCallAlerting == (int)ELC_CAN_CALL_ALERTING && kCallActive == (int)ELC_CAN_CALL_ACTIVE &&
                  kCallWaiting == (int)ELC_CAN_CALL_WAITING && kCallHeld == (int)ELC_CAN_CALL_HELD,
              "call states");
static_assert(kPhoneCmdAnswer == (int)ELC_CAN_PHONE_CMD_ANSWER && kPhoneCmdEnd == (int)ELC_CAN_PHONE_CMD_END &&
                  kPhoneCmdConfirm == (int)ELC_CAN_PHONE_CMD_CONFIRM && kPhoneCmdForget == (int)ELC_CAN_PHONE_CMD_FORGET &&
                  kPhoneCmdAudioRoute == (int)ELC_CAN_PHONE_CMD_AUDIO_ROUTE && kPhoneCmdMediaVolume == (int)ELC_CAN_PHONE_CMD_MEDIA_VOLUME,
              "phone commands");
static_assert(kPhoneMaxPaired == (int)ELC_CAN_PHONE_MAX_PAIRED && kPhoneTextMax == (int)ELC_CAN_PHONE_TEXT_MAX, "phone sizes");

static ElcCan s_can;
static uint8_t s_can_slot = 0;  // range index: node - 0x10 (the "slot" in the audio messages)
static uint8_t s_can_text_serial = 0;
static char s_can_text_sent[kMaxTrackLen + 1];
static uint32_t s_can_text_ms = 0;

/* Album art over CAN: a display asks for an art id and variant from a chunk on (ART_REQUEST); the bridge answers
 * with ART_INFO and then streams ART_DATA to the end. Variants: the phone's original JPEG, a small (kArtSmallSide²) JPEG (the
 * center shows it first, then steps up to the original) and a 48x48 4-bit grey image (console OLED); the small
 * ones are made once per cover, and the cover is announced only after that. Art frames lose arbitration to the
 * sensor and cluster frames, but they outrank heartbeats and the other slots (knob events from a console card),
 * so the streams are paced to one frame per kArtFrameUs in total: about a third of the bus. Smallest first. */
static constexpr uint32_t kArtFrameUs = 800;
static constexpr size_t kArtSmallCap = kArtSmallSide >= 128 ? 16 * 1024 : 4 * 1024;  // 128 px ~2–5 KB, 64 px ~1.5–2.5 KB
static uint32_t s_srv_credit_us = 0;
struct ArtStream {
  uint8_t id = 0;       // art id being streamed (0 = idle)
  uint16_t next = 0;    // next chunk to send
  bool info = false;    // ART_INFO still to send
};
static ArtStream s_srv[3];                  // index = ElcCanArtVariant
static uint8_t s_var_id = 0;                // art id the variants below were made from (0 = none)
static uint8_t s_var_made = 0;              // kArtHave* bits
static uint8_t *s_var_jpg = nullptr;        // small JPEG
static size_t s_var_jpg_len = 0;
static uint8_t s_var_gray[kArtGrayBytes];

/** The ready cover, announced on CAN only once its small variants have been made (or could not be). */
static uint8_t can_art_announced() {
  return (s_art_state == kArtReady && s_var_id == s_art_id) ? s_art_id : 0;
}

/** Bytes of one variant of the announced cover; size 0 = not available. */
static const uint8_t *art_variant(uint8_t v, uint32_t *size) {
  *size = 0;
  if (!can_art_announced()) return nullptr;
  if (v == ELC_CAN_ART_ORIGINAL && s_art) {
    *size = (uint32_t)s_art_rx;
    return s_art;
  }
  if (v == ELC_CAN_ART_JPEG_SMALL && (s_var_made & kArtHaveJpegSmall)) {
    *size = (uint32_t)s_var_jpg_len;
    return s_var_jpg;
  }
  if (v == ELC_CAN_ART_GRAY48 && (s_var_made & kArtHaveGray48)) {
    *size = (uint32_t)kArtGrayBytes;
    return s_var_gray;
  }
  return nullptr;
}

/** Make the small variants of a newly ready cover (tens of ms, once per cover). */
static void art_make_small() {
  if (s_art_state != kArtReady || s_var_id == s_art_id) {
    return;
  }
  // The small JPEG buffer stays allocated once made, so only take it when the variants can be made at all.
  if (!s_var_jpg && (psramFound() || ESP.getFreeHeap() >= kArtSmallCap + art_variants_work_bytes() + kArtHeapReserve)) {
    s_var_jpg = static_cast<uint8_t *>(art_alloc(kArtSmallCap));
  }
  const uint32_t t0 = millis();
  s_var_made = art_make_variants(s_art, s_art_rx, s_var_jpg, s_var_jpg ? kArtSmallCap : 0, &s_var_jpg_len, s_var_gray);
  s_var_id = s_art_id;
  Serial.printf("cover art #%u variants: %dpx JPEG %s (%u bytes), 48px grey %s, %lu ms, heap %u (needs %u + %u reserve)\n",
                (unsigned)s_art_id, kArtSmallSide, (s_var_made & kArtHaveJpegSmall) ? "ok" : "none", (unsigned)s_var_jpg_len,
                (s_var_made & kArtHaveGray48) ? "ok" : "none", (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap(),
                (unsigned)art_variants_work_bytes(), (unsigned)kArtHeapReserve);
}

static void can_on_frame(const ElcCanFrame &f, void *) {
  ElcCanPhoneCommandMsg pc;
  if (elc_can_decode_phone_command(f, pc)) {
    if (pc.slot == s_can_slot) {
      const bool ok = run_phone_command(pc.command, pc.arg, pc.list_serial, true);
      Serial.printf("phone command %u (%u) from node 0x%02X: %s\n", (unsigned)pc.command, (unsigned)pc.arg,
                    (unsigned)pc.from_node, ok ? "ok" : "not done");
    }
    return;
  }
  ElcCanArtRequest r;
  if (!elc_can_decode_art_request(f, r) || r.slot != s_can_slot || r.art_id == 0 || r.variant > ELC_CAN_ART_GRAY48) {
    return;  // theme etc.: nothing on this board uses it (heartbeats are tracked by ElcCan)
  }
  if (r.art_id != can_art_announced()) {
    return;  // stale request: the transport frame already announces the current id
  }
  ArtStream &st = s_srv[r.variant];
  st.id = r.art_id;
  st.next = r.first_chunk;
  st.info = true;  // a variant we do not have is answered with size 0
}

static void can_serve_art() {
  static uint32_t s_last_us = 0;
  const uint32_t now_us = micros();
  s_srv_credit_us += now_us - s_last_us;
  s_last_us = now_us;
  if (s_srv_credit_us > 8 * kArtFrameUs) {
    s_srv_credit_us = 8 * kArtFrameUs;  // no bursts after an idle spell
  }
  size_t room = s_can.txFree();
  if (room) {
    room--;  // leave one place for transport / heartbeat
  }
  static const uint8_t kOrder[3] = {ELC_CAN_ART_GRAY48, ELC_CAN_ART_JPEG_SMALL, ELC_CAN_ART_ORIGINAL};
  for (uint8_t v : kOrder) {
    ArtStream &st = s_srv[v];
    if (!st.id) continue;
    uint32_t size = 0;
    const uint8_t *img = art_variant(v, &size);
    if (st.id != can_art_announced()) {
      st.id = 0;  // the cover changed under us; the requester will ask for the new one
      continue;
    }
    ElcCanFrame f;
    if (st.info) {
      if (!room) return;
      elc_can_encode_art_info(s_can_slot, st.id, v, size, f);
      if (!s_can.send(f)) return;
      st.info = false;
      room--;
      if (!size) {
        st.id = 0;
        continue;
      }
    }
    const uint16_t chunks = elc_can_art_chunks(size);
    while (room && s_srv_credit_us >= kArtFrameUs && st.next < chunks) {
      elc_can_encode_art_data(s_can_slot, st.id, v, st.next, img, size, f);
      if (!s_can.send(f)) return;
      st.next++;
      room--;
      s_srv_credit_us -= kArtFrameUs;
    }
    if (st.next >= chunks) {
      st.id = 0;
    }
    if (!room || s_srv_credit_us < kArtFrameUs) return;
  }
}

static void can_begin() {
  uint8_t node = 0;
  if (ELC_CAN_CARD_SLOT_PIN >= 0) {
    // Rev 2 audio card: the node follows the slot (docs/hardware/accessory_ports.md, "Node numbers").
    const uint8_t slot = elc_can_read_card_slot(ELC_CAN_CARD_SLOT_PIN);
    node = elc_can_node_for_card_slot(slot);
    if (node) {
      Serial.printf("CAN: card slot %u -> node 0x%02X\n", (unsigned)slot, (unsigned)node);
    } else if (ELC_CAN_BENCH_NODE) {
      node = ELC_CAN_BENCH_NODE;
      Serial.printf("CAN: no slot (no main board?), bench node 0x%02X\n", (unsigned)node);
    } else {
      Serial.println("CAN: no slot read on the slot ID pin (no main board?): staying off the bus");
      return;
    }
  } else {
    node = (uint8_t)(ELC_CAN_NODE_SLOT0 + elc_can_read_slot_id(ELC_CAN_SLOT_ID0_PIN, ELC_CAN_SLOT_ID1_PIN));
  }
  s_can_slot = (uint8_t)(node - ELC_CAN_NODE_SLOT0);
  s_can.onFrame(can_on_frame, nullptr);
  const bool ok = s_can.begin(ELC_CAN_TX_PIN, ELC_CAN_RX_PIN, node, ElcCanNodeType::AudioBridge);
  Serial.printf("CAN: %s, node 0x%02X (TX=%d RX=%d)\n", ok ? "up" : "off", (unsigned)node, ELC_CAN_TX_PIN,
                ELC_CAN_RX_PIN);
}

/** Now-playing text (on change and every second, so a late listener fills in) and transport (every call). */
static void can_publish() {
  char text[kMaxTrackLen + 1];
  ElcCanAudioTransport t;
  portENTER_CRITICAL(&s_track_mux);
  memcpy(text, s_track, sizeof(text));
  text[kMaxTrackLen] = '\0';
  t.elapsed_s = s_elapsed_s;
  t.duration_s = s_duration_s;
  t.volume = s_vol_0_15;
  portEXIT_CRITICAL(&s_track_mux);
  t.connected = a2dp_sink && a2dp_sink->is_connected();
  t.art_id = can_art_announced();

  const uint32_t now = millis();
  const bool changed = strncmp(text, s_can_text_sent, sizeof(text)) != 0;
  if (changed) {
    memcpy(s_can_text_sent, text, sizeof(text));
    s_can_text_serial = (uint8_t)((s_can_text_serial + 1) & 0x0F);
  }
  t.text_serial = s_can_text_serial;
  ElcCanFrame f;
  elc_can_encode_audio_transport(s_can_slot, t, f);
  s_can.send(f);
  if (changed || now - s_can_text_ms >= 1000u) {
    s_can_text_ms = now;
    ElcCanFrame chunks[ELC_CAN_AUDIO_TEXT_CHUNKS];
    elc_can_encode_audio_text(s_can_slot, s_can_text_serial, text, chunks);
    for (const ElcCanFrame &c : chunks) {
      s_can.send(c);
    }
  }
}

/** One phone text, only when the TX queue has room for all of it (it is repeated anyway). */
static bool can_send_phone_text(uint8_t kind, uint8_t serial, const char *text) {
  if (s_can.txFree() < ELC_CAN_PHONE_TEXT_CHUNKS + 1) return false;
  ElcCanFrame chunks[ELC_CAN_PHONE_TEXT_CHUNKS];
  elc_can_encode_phone_text(s_can_slot, kind, serial, text, chunks);
  for (const ElcCanFrame &c : chunks) s_can.send(c);
  return true;
}

/** Phone status on change and every second; texts and the pairing code on their schedules (can_bus.md, "Phone").
 *  Runs every loop pass: a change (a call ringing) goes out within milliseconds. */
static void can_publish_phone() {
  static uint32_t s_sent_changes = 0, s_status_ms = 0, s_caller_ms = 0, s_conn_ms = 0, s_pairname_ms = 0,
                  s_pairing_ms = 0, s_name_ms = 0;
  static uint8_t s_caller_serial = 0xFF, s_list_serial = 0xFF, s_name_next = 0, s_names_due = 0;
  const uint32_t now = millis();
  const uint32_t changes = phone_changes();
  const bool due = changes != s_sent_changes || now - s_status_ms >= 1000u;
  if (!due && now - s_name_ms < 100u) return;  // nothing new: look at the slower texts ten times a second
  PhoneState st;
  phone_get(&st);
  if (due) {
    ElcCanPhoneStatus s;
    s.flags = st.flags;
    s.call = st.call;
    s.call_s = st.call_s;
    s.signal = st.signal;
    s.battery = st.battery;
    s.caller_serial = st.caller_serial;
    s.list_serial = st.list_serial;
    s.paired = st.paired;
    s.connected_index = st.connected_index;
    s.call_volume = st.call_volume;
    ElcCanFrame f;
    elc_can_encode_phone_status(s_can_slot, s, f);
    if (s_can.send(f)) {
      s_sent_changes = changes;
      s_status_ms = now;
    }
  }
  if ((st.flags & (kPhonePairing | kPhoneConfirm)) && now - s_pairing_ms >= 500u) {
    ElcCanFrame f;
    elc_can_encode_phone_pairing(s_can_slot, (st.flags & kPhoneConfirm) ? st.passkey : 0, st.pair_left, f);
    if (s_can.send(f)) s_pairing_ms = now;
  }
  if (st.call != kCallIdle || st.caller_serial != s_caller_serial) {
    if ((st.caller_serial != s_caller_serial || now - s_caller_ms >= 1000u) &&
        can_send_phone_text(ELC_CAN_PHONE_TEXT_CALLER, st.caller_serial, st.caller)) {
      s_caller_serial = st.caller_serial;
      s_caller_ms = now;
    }
  }
  if ((st.flags & kPhoneConfirm) && now - s_pairname_ms >= 1000u &&
      can_send_phone_text(ELC_CAN_PHONE_TEXT_PAIRING, st.pairing_serial, st.pairing_name)) {
    s_pairname_ms = now;
  }
  if ((st.flags & kPhoneConnected) && now - s_conn_ms >= 2000u &&
      can_send_phone_text(ELC_CAN_PHONE_TEXT_CONNECTED, st.connected_serial, st.connected_name)) {
    s_conn_ms = now;
  }
  // Paired names: all of them right after the list changes (one per pass, as the queue allows), then one every 0.5 s.
  if (st.list_serial != s_list_serial) {
    s_list_serial = st.list_serial;
    s_name_next = 0;
    s_names_due = st.paired;
  }
  if (st.paired && (s_names_due || now - s_name_ms >= 500u)) {
    if (s_name_next >= st.paired) s_name_next = 0;
    if (can_send_phone_text((uint8_t)(ELC_CAN_PHONE_TEXT_PAIRED + s_name_next), st.list_serial, st.paired_name[s_name_next])) {
      s_name_next++;
      s_name_ms = now;
      if (s_names_due) s_names_due--;
    }
  } else if (!st.paired) {
    s_names_due = 0;
    s_name_ms = now;
  }
}
#endif

/** Bench console: phone commands and status without the console board (`help`). */
static void serial_poll() {
  static char line[64];
  static size_t n = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (n < sizeof(line) - 1) line[n++] = c;
      continue;
    }
    line[n] = '\0';
    n = 0;
    char word[16] = {0}, rest[16] = {0};
    if (sscanf(line, "%15s %15s", word, rest) < 1) continue;
    const int num = atoi(rest);
    const bool on = strcmp(rest, "off") != 0 && strcmp(rest, "no") != 0 && strcmp(rest, "0") != 0 &&
                    strcmp(rest, "phone") != 0;
    bool ok = true;
    if (!strcmp(word, "phone")) {
      phone_print_status();
      continue;
    } else if (!strcmp(word, "pair")) {
      ok = run_phone_command(kPhoneCmdPairing, rest[0] ? on : 1, 0, false);
    } else if (!strcmp(word, "confirm")) {
      ok = run_phone_command(kPhoneCmdConfirm, on, 0, false);
    } else if (!strcmp(word, "answer")) {
      ok = run_phone_command(kPhoneCmdAnswer, 0, 0, false);
    } else if (!strcmp(word, "end")) {
      ok = run_phone_command(kPhoneCmdEnd, 0, 0, false);
    } else if (!strcmp(word, "mute")) {
      ok = run_phone_command(kPhoneCmdMicMute, rest[0] ? on : 1, 0, false);
    } else if (!strcmp(word, "vol")) {
      ok = run_phone_command(kPhoneCmdCallVolume, (uint8_t)num, 0, false);
    } else if (!strcmp(word, "connect")) {
      ok = run_phone_command(kPhoneCmdConnect, (uint8_t)num, 0, false);
    } else if (!strcmp(word, "disconnect")) {
      ok = run_phone_command(kPhoneCmdDisconnect, 0, 0, false);
    } else if (!strcmp(word, "forget")) {
      ok = run_phone_command(kPhoneCmdForget, (uint8_t)num, 0, false);
    } else if (!strcmp(word, "redial")) {
      ok = run_phone_command(kPhoneCmdRedial, 0, 0, false);
    } else if (!strcmp(word, "voice")) {
      ok = run_phone_command(kPhoneCmdVoice, rest[0] ? on : 1, 0, false);
    } else if (!strcmp(word, "waiting")) {
      ok = run_phone_command(kPhoneCmdCallWaiting, (uint8_t)num, 0, false);
    } else if (!strcmp(word, "route")) {
      ok = run_phone_command(kPhoneCmdAudioRoute, strcmp(rest, "phone") != 0, 0, false);
    } else if (!strcmp(word, "play") || !strcmp(word, "next") || !strcmp(word, "prev")) {
      ok = run_phone_command(kPhoneCmdMedia, word[0] == 'n' ? 1 : (word[1] == 'r' ? 2 : 0), 0, false);
    } else {
      Serial.println("Commands: phone | pair [on|off] | confirm yes|no | answer | end | mute [on|off] | vol 0-15 |");
      Serial.println("  connect <n> | disconnect | forget <n> | redial | voice [on|off] | waiting 0|1|2 | route car|phone |");
      Serial.println("  play | next | prev      (<n> = the index in `phone`)");
      continue;
    }
    Serial.println(ok ? "ok" : "not done (no phone, no call, or a bad index)");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("esp32_audio_bridge: A2DP sink + AVRCP + hands-free (HFP) + CAN"));
  Serial.printf("Audio: I2S -> external DAC (BCK=%d WS=%d DOUT=%d), mic ADC DIN=%d MCLK=%d\n", I2S_PIN_BCK, I2S_PIN_WS,
                I2S_PIN_DATA, I2S_PIN_DIN, I2S_PIN_MCLK);

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
    audio_output = new I2SStream();
    auto cfg = audio_output->defaultConfig(TX_MODE);
    cfg.pin_bck = I2S_PIN_BCK;
    cfg.pin_ws = I2S_PIN_WS;
    cfg.pin_data = I2S_PIN_DATA;
    if (!audio_output->begin(cfg)) {
      Serial.println(F("audio_output.begin (I2S) failed"));
    }
    a2dp_output = new CallGateStream(*audio_output);
    CallAudioPins pins;
    pins.bck = I2S_PIN_BCK;
    pins.ws = I2S_PIN_WS;
    pins.dout = I2S_PIN_DATA;
    pins.din = I2S_PIN_DIN;
    pins.mclk = I2S_PIN_MCLK;
    pins.mic_right = MIC_RIGHT_CHANNEL != 0;
    call_audio_begin(audio_output, pins);
  }
#if DAC_UNMUTE_PIN >= 0
  pinMode(DAC_UNMUTE_PIN, OUTPUT);  // the carrier's DACs stay soft-muted until the I2S clocks run
  digitalWrite(DAC_UNMUTE_PIN, HIGH);
#endif

#if AUDIO_BRIDGE_I2C
  /* I2C slave before A2DP/BT: host may poll 0x2A as soon as wires are live; avoid missing early reads. */
  Wire.begin(static_cast<uint8_t>(I2C_SLAVE_ADDR), I2C_SDA_PIN, I2C_SCL_PIN, 100000U);
  Wire.onReceive(onReceive);
  Wire.onRequest(onRequest);
#endif

#if CLUSTER_THEME_I2C_ENABLE
  Wire1.begin(static_cast<uint8_t>(CLUSTER_THEME_I2C_ADDR), CLUSTER_THEME_SDA, CLUSTER_THEME_SCL, 100000U);
  Wire1.onReceive(on_cluster_theme_receive);
#endif

  s_art = static_cast<uint8_t *>(art_alloc(kArtMax));  // PSRAM when the board has it
  a2dp_sink = new CoverArtSink(*a2dp_output);
  a2dp_sink->set_avrc_metadata_attribute_mask(
      ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM |
      ESP_AVRC_MD_ATTR_TRACK_NUM | ESP_AVRC_MD_ATTR_NUM_TRACKS | ESP_AVRC_MD_ATTR_GENRE |
      ESP_AVRC_MD_ATTR_PLAYING_TIME | ESP_AVRC_MD_ATTR_COVER_ART);
  a2dp_sink->set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink->set_avrc_rn_playstatus_callback(avrc_play_status_cb);
  /* Default in library was 10s between position notifications — too coarse for a dash. */
  a2dp_sink->set_avrc_rn_play_pos_callback(avrc_play_pos_cb, 1);
  a2dp_sink->set_avrc_rn_volumechange(avrc_volume_cb);
  a2dp_sink->set_on_connection_state_changed(a2dp_connection_cb);
  a2dp_sink->set_auto_reconnect(true);
  phone_begin(a2dp_sink);
  /* Classic BT only: hand the BLE controller's memory back to the heap before the controller starts (the album-art
   * variants need ~37 KB while a phone is streaming). Must run before start(); it cannot be undone. */
  const uint32_t heap_before_ble = ESP.getFreeHeap();
  const esp_err_t ble_rel = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  Serial.printf("BLE memory release: %s, +%d bytes heap\n", esp_err_to_name(ble_rel), (int)(ESP.getFreeHeap() - heap_before_ble));
  a2dp_sink->start(BT_DEVICE_NAME);
  phone_after_start();
#if AUDIO_BRIDGE_CAN
  can_begin();
#endif

  Serial.print(F("BT name: "));
  Serial.println(BT_DEVICE_NAME);
  Serial.printf("cover art buffer %s, heap %u bytes free, PSRAM %u KB free (%dpx art copy)\n", s_art ? "ok" : "FAILED",
                (unsigned)ESP.getFreeHeap(), (unsigned)(ESP.getFreePsram() / 1024), kArtSmallSide);
#if AUDIO_BRIDGE_I2C
  Serial.print(F("I2C metadata 0x"));
  Serial.println(I2C_SLAVE_ADDR, HEX);
#else
  Serial.println(F("I2C off (bench only): now-playing and album art go over CAN"));
#endif
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
#if AUDIO_BRIDGE_CAN
  // Drain CAN often (the bus carries ~100 sensor frames/s) and keep album art flowing; the rest runs
  // every 250 ms.
  s_can.poll();
  can_publish_phone();
  can_serve_art();
  static uint32_t s_phone_ms = 0;
  if (millis() - s_phone_ms >= 50u) {
    s_phone_ms = millis();
    phone_loop();
    serial_poll();
  }
  static uint32_t s_last_tick_ms = 0;
  if (millis() - s_last_tick_ms < 250u) {
    delay(2);
    return;
  }
  s_last_tick_ms = millis();
#endif
  // The AVRCP play-status notification only fires on a change, so after a reboot mid-song it never
  // comes. The A2DP stream itself is the reliable signal: audio flowing = playing.
  const bool streaming = a2dp_sink && a2dp_sink->get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED;
  // Keep the elapsed estimate in the reply buffer moving between AVRCP events.
  portENTER_CRITICAL(&s_track_mux);
  if (streaming != s_playing) {
    apply_elapsed_estimate_unlocked();
    s_playing = streaming;
    anchor_position_unlocked(s_elapsed_s);
  }
  rebuild_snapshot_unlocked();
  portEXIT_CRITICAL(&s_track_mux);
  cover_art_poll();
#if AUDIO_BRIDGE_CAN
  art_make_small();
#endif
#if AUDIO_BRIDGE_SERIAL_METADATA
  serial_log_metadata_line();
#endif
#if AUDIO_BRIDGE_CAN
  can_publish();
#else
  phone_loop();
  serial_poll();
  delay(250);
#endif
}
