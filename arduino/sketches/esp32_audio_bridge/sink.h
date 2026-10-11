#pragma once

/*
 * The audio card as the audio sink (docs/protocols/can_channels.md, AUDIO_CMD and NET RAIL): it owns which source
 * plays and the master volume, and runs the I2S rail's RET.
 *
 *   sources    Bluetooth (its own A2DP), the radio (a radio card's audio on RET), aux in (the card's ADC), off.
 *              SOURCE picks one; the radio learns it was chosen from STATUS and asks for RET. Bluetooth pauses when
 *              the sink leaves it and plays again when it comes back, if it was playing. The choice is kept.
 *   volume     VOLUME (set / step / mute) on the 0..15 curve the console's knob steps through; STATUS says 0..100.
 *   media      MEDIA for Bluetooth goes to the phone; for the radio it is forwarded to the radio card.
 *   RET        NET RAIL: one holder at a time. A voice card outranks any other card, the radio (a continuous source)
 *              ranks below everything; a holder whose heartbeat stops loses it.
 *   STATUS     at 1 Hz and on change.
 *   its page   Menu > Accessories > Audio card (UI): chime volume, fader, balance and the Equalizer (a preset and ten
 *              bands), at the fixed item numbers of elc_chan_msg.h (ELC_SINK_ITEM_*), all settings the center backs up.
 *
 * No Arduino or ESP-IDF dependencies: the firmware gives it a SinkIo and feeds it transfers and the clock, so it runs
 * in the host tests (firmware/esp32_classic_bt_audio/test/host/test_sink.cpp).
 */

#include <stddef.h>
#include <stdint.h>

#include <elc_chan.h>
#include <elc_chan_msg.h>

/** What the sink is set to (kept in NVS by the firmware). */
struct SinkSettings {
  uint8_t source = ELC_SRC_BLUETOOTH;  // ELC_SRC_*: off (0), Bluetooth, radio, aux
  uint8_t volume = 8;                  // 0..15
  uint8_t chime_volume = 8;            // 1..15
  int8_t fader = 0, balance = 0;       // -10..10
  uint8_t eq_preset = ELC_EQ_PRESET_FLAT;
  int8_t eq_custom[ELC_EQ_BANDS] = {0};  // the Custom preset's curve
  bool factory = true;                 // never changed (ANNOUNCE flag bit0: the center may restore its backup)
};

/** What the sink asks of the firmware. */
class SinkIo {
 public:
  virtual ~SinkIo() = default;
  /** A transfer on a channel (AUDIO_CMD, UI, NET). */
  virtual void send(uint16_t base, const ElcChWriter &w) = 0;
  /** ELC_MEDIA_PLAY / PAUSE / TOGGLE / NEXT / PREV to the phone. */
  virtual void bt_media(uint8_t action) = 0;
  /** The phone plays (its AVRCP play status). */
  virtual bool bt_playing() = 0;
  /** The node's heartbeat was seen in the last 3 s. */
  virtual bool node_alive(uint8_t node) = 0;
  virtual uint8_t chimes_active() = 0;
  virtual bool chimes_ducking() = 0;
  /** The chime volume changed: the soft chime once at it, so it can be heard. */
  virtual void chime_preview(uint8_t volume) = 0;
  /** Something the output depends on changed (music, the RET holder, volume, mute, EQ, fader): read it back. */
  virtual void apply() = 0;
  /** The settings changed and have been quiet for 2 s: save them. */
  virtual void save(const SinkSettings &s) = 0;
  virtual void log(const char *line) = 0;
};

/** What the output plays, from the source and the RET holder. */
enum class SinkMusic : uint8_t { Off, Bluetooth, Rail, Aux };

class AudioSink {
 public:
  struct Caps {
    bool aux = false;   // an ADC: aux in can be a source
    bool rear = false;  // the rear DAC: fader is offered
    uint8_t slot = 0;   // the card's slot (in RAIL answers)
  };

  void begin(ElcChannels *ch, SinkIo *io, const Caps &caps, const SinkSettings &saved, uint32_t now_ms);

  /** Every complete transfer (AUDIO_CMD VOLUME / MEDIA / SOURCE, NET RAIL, UI SET / VIEW). PLAY / STOP are the
   *  firmware's (the chimes). */
  void transfer(const ElcChTransfer &t, uint32_t now_ms);
  /** Often: STATUS, the RET holder's heartbeat, the radio fallback after boot, saving. */
  void tick(uint32_t now_ms);
  /** QUERY bit3 and boot: the page's ITEMs and their values. */
  void describe_ui();

  // ---- the old Bluetooth frames (can_bus.md, phone commands), which carry the volume as 0..15
  void set_volume15(uint8_t v, uint32_t now_ms);

  // ---- state for the engine and the serial console
  uint8_t source() const { return _s.source; }
  SinkMusic music() const;
  /** The card holding RET talks over the music (any holder but the radio). */
  bool rail_voice() const { return _holder && _holder != radio(); }
  uint8_t volume15() const { return _s.volume; }
  bool muted() const { return _muted; }
  /** The equaliser's curve now: the preset's, or the Custom one. */
  void eq_db(int8_t out[ELC_EQ_BANDS]) const;
  const SinkSettings &settings() const { return _s; }
  uint8_t holder() const { return _holder; }
  uint8_t radio() const;
  /** One line for the serial console. */
  void describe(char *out, size_t n) const;

 private:
  void set_source(uint8_t code, uint32_t now_ms, const char *why);
  void source_next(uint32_t now_ms);
  void volume_cmd(const ElcAudioVolume &v, uint32_t now_ms);
  void media(uint8_t action, uint8_t value);
  void rail(const ElcNetRail &r, uint8_t src, uint32_t now_ms);
  void rail_send(uint8_t dest, uint8_t state);
  uint8_t rank(uint8_t node) const;
  void ui_set(const ElcUiSet &s, uint8_t asker, uint32_t now_ms);
  void ui_value(uint8_t item);
  void send_values(uint8_t group);
  void send_status(uint32_t now_ms);
  void status_tuple(uint8_t out[7]) const;
  void changed(uint32_t now_ms, bool setting);
  void logf(const char *fmt, ...);

  ElcChannels *_ch = nullptr;
  SinkIo *_io = nullptr;
  Caps _caps;
  SinkSettings _s;
  bool _muted = false;
  bool _bt_was_playing = false;
  uint8_t _holder = 0;
  uint32_t _holder_ms = 0;
  uint32_t _boot_ms = 0;
  bool _radio_wait = false;  // booted to the radio: Bluetooth if none is heard soon
  uint8_t _sent[7] = {0};
  bool _sent_any = false;
  uint32_t _status_ms = 0;
  uint32_t _dirty_ms = 0;    // a setting changed (saved 2 s after the last change)
  uint8_t _view = ELC_UI_VIEW_CLOSED;
  uint32_t _view_ms = 0;
};
