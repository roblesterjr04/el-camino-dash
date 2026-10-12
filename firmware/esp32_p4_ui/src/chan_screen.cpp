#include "chan_screen.h"

#include <stdio.h>
#include <string.h>

#include <accessory_can.h>

#if defined(ARDUINO)
#include <Arduino.h>
#endif

namespace {

ElcChannels s_ch;
bool s_begun = false;
uint8_t s_screen = ELC_CAN_SCREEN_CENTER;
void (*s_on_transfer)(const ElcChTransfer &, void *) = nullptr;
void *s_on_transfer_ctx = nullptr;
void (*s_on_describe)(uint8_t, void *) = nullptr;
void *s_on_describe_ctx = nullptr;
ElcSysVehicle s_vehicle;
ElcSysPrefs s_prefs;
bool s_have_vehicle = false, s_have_prefs = false;
uint32_t s_vehicle_ms = 0, s_now = 0;
uint32_t s_begin_ms = 0;
bool s_queried = false;  // the center's QUERY to everyone after boot
// AUDIO_CMD, as the console follows it: the audio card's STATUS (its source) and NOW PLAYING for the radio
ElcAudioStatus s_audio;
ElcAudioNow s_radio_now;
uint32_t s_audio_ms = 0, s_radio_now_ms = 0;

void on_transfer(const ElcChTransfer &t, void *) {
  accessory_can_transfer(t, s_now);
  if (t.base == ELC_CH_SYSTEM) {
    ElcSysVehicle v;
    ElcSysPrefs p;
    if (elc_sys_decode(t, v)) {
      s_vehicle = v;
      s_have_vehicle = true;
      s_vehicle_ms = s_now;
    } else if (elc_sys_decode(t, p)) {
      s_prefs = p;
      s_have_prefs = true;
    }
  } else if (t.base == ELC_CH_AUDIO_CMD) {
    ElcAudioStatus st;
    ElcAudioNow np;
    if (elc_audio_decode(t, st)) {
      if (t.src == s_ch.findNode(ELC_TYPE_AUDIO_SINK)) {  // only the audio card's
        s_audio = st;
        s_audio_ms = s_now ? s_now : 1;
      }
    } else if (elc_audio_decode(t, np) && np.source == ELC_SRC_RADIO) {
      // from the audio card for its source, or the radio card itself
      if (t.src == s_audio.source_node || t.src == s_ch.findNode(ELC_TYPE_RADIO)) {
        s_radio_now = np;
        s_radio_now_ms = s_now ? s_now : 1;
      }
    }
  } else if (t.base == ELC_CH_LOG && s_screen == ELC_CAN_SCREEN_CENTER) {
    ElcLogLine l;
    if (elc_log_decode(t, l)) {
      static const char *const kLevel[4] = {"debug", "info", "warn", "error"};
      const char *nm = s_ch.nodeInfo(t.src).announced ? s_ch.nodeInfo(t.src).a.name : "";
#if defined(ARDUINO)
      Serial.printf("[log 0x%02X %s] %s: %s\n", (unsigned)t.src, nm, kLevel[l.level & 3], l.text);
#else
      (void)nm;
      (void)kLevel;
#endif
    }
  }
  if (s_on_transfer) s_on_transfer(t, s_on_transfer_ctx);
}

void on_describe(uint8_t what, void *) {
  if (s_on_describe) s_on_describe(what, s_on_describe_ctx);
}

uint32_t unique_id() {
#if defined(ARDUINO_ARCH_ESP32)
  const uint64_t mac = ESP.getEfuseMac();
  return (uint32_t)(mac >> 16) ^ (uint32_t)mac;
#else
  return 0;
#endif
}

}  // namespace

void chan_screen_begin(ElcCan &can, uint8_t screen) {
  s_screen = screen;
  const bool center = screen == ELC_CAN_SCREEN_CENTER;
  ElcNetAnnounce me;
  me.type = ELC_TYPE_DISPLAY;
  me.product = ELC_PRODUCT_DISPLAY;
  me.fw = ELC_CAN_FW_VERSION;
  me.uid = unique_id();
  // Every screen can be the menu screen: it sends UI SET / VIEW and reads every accessory's UI, alerts and
  // sensors. The center also owns SYSTEM, the core SENSOR readings and the alert manager, and logs LOG lines.
  me.produces = elc_ch_bit(ELC_CH_UI);
  me.consumes = elc_ch_bit(ELC_CH_UI) | elc_ch_bit(ELC_CH_ALERT) | elc_ch_bit(ELC_CH_SENSOR) | elc_ch_bit(ELC_CH_SYSTEM) |
                elc_ch_bit(ELC_CH_INPUT) | elc_ch_bit(ELC_CH_AUDIO_CMD);
  if (center) {
    me.produces |= elc_ch_bit(ELC_CH_SYSTEM) | elc_ch_bit(ELC_CH_SENSOR) | elc_ch_bit(ELC_CH_ALERT);
    me.consumes |= elc_ch_bit(ELC_CH_LOG);
  }
  snprintf(me.name, sizeof(me.name), "%s screen",
           center ? "Center" : (screen == ELC_CAN_SCREEN_LEFT ? "Left" : "Right"));
  can.attachChannels(&s_ch, me);
  s_ch.onTransfer(on_transfer, nullptr);
  s_ch.onDescribe(on_describe, nullptr);
  accessory_can_begin(&s_ch);
  s_begun = true;
}

void chan_screen_poll(uint32_t now_ms) {
  s_now = now_ms;
  if (!s_begun) return;
  if (!s_begin_ms) s_begin_ms = now_ms ? now_ms : 1;
  // The center asks every node for its announce and descriptions once it is up (they answer after node x 8 ms),
  // so it learns about nodes that booted before it. 500 ms in, after its own boot ANNOUNCE.
  if (s_screen == ELC_CAN_SCREEN_CENTER && !s_queried && now_ms - s_begin_ms >= 500) {
    s_queried = true;
    s_ch.query(0x00, ELC_NET_Q_ALL);
  }
  accessory_can_poll(now_ms);
}

ElcChannels *chan_screen() { return s_begun ? &s_ch : nullptr; }

void chan_screen_on_transfer(void (*fn)(const ElcChTransfer &, void *), void *ctx) {
  s_on_transfer = fn;
  s_on_transfer_ctx = ctx;
}

void chan_screen_on_describe(void (*fn)(uint8_t, void *), void *ctx) {
  s_on_describe = fn;
  s_on_describe_ctx = ctx;
}

bool chan_screen_vehicle(ElcSysVehicle *out, uint32_t *age_ms) {
  if (!s_have_vehicle) return false;
  if (out) *out = s_vehicle;
  if (age_ms) *age_ms = s_now - s_vehicle_ms;
  return true;
}

bool chan_screen_radio(ElcAudioNow *now) {
  if (!s_audio_ms || s_now - s_audio_ms > 3000 || s_audio.source != ELC_SRC_RADIO) return false;
  const bool have = s_radio_now_ms && s_now - s_radio_now_ms < 5000;
  if (now) *now = have ? s_radio_now : ElcAudioNow();
  if (now && !have) now->source = 0;  // the radio plays, but nothing from it yet
  return true;
}

bool chan_screen_prefs(ElcSysPrefs *out) {
  if (!s_have_prefs) return false;
  if (out) *out = s_prefs;
  return true;
}

bool chan_screen_command(const char *line, char *reply, size_t n) {
  if (strncmp(line, "chan", 4) != 0 || (line[4] && line[4] != ' ')) return false;
  if (!s_begun) {
    snprintf(reply, n, "channels: not started");
    return true;
  }
  if (strcmp(line, "chan query") == 0) {
    s_ch.query(0x00, ELC_NET_Q_ALL);
    snprintf(reply, n, "QUERY sent to every node");
    return true;
  }
  char a[400], b[400];
  s_ch.describe(a, sizeof(a));
  accessory_can_describe(b, sizeof(b));
  snprintf(reply, n, "%s\n%s", a, b);
  return true;
}
