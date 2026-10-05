#pragma once

/*
 * The alert chimes: the built-in sounds 2, 3 and 4 of the AUDIO_CMD registry (docs/protocols/can_channels.md, Sounds),
 * synthesised as struck bells, so there is nothing to store and they play at whatever rate the I2S runs.
 *
 *   2 soft chime (advisory)  one mellow strike, A5                                  1.2 s
 *   3 caution chime          "ding-dong", E6 then C6                                1.5 s
 *   4 warning chime          three quick bright strikes on A6, twice a cycle         2.0 s
 *
 * The length is one play including its trailing silence, so a sound repeated (PLAY repeat > 1, or 0 = until STOP)
 * keeps its rhythm; tools/cards_sim.py uses the same lengths.
 *
 * They play at the chime volume (set_volume), whatever the music's volume. Only the voices at the highest priority
 * playing are heard; lower ones keep their time (and their repeat count)
 * silently and are heard again when the higher ones end. A voice of priority 2 or more (the alerts) ducks the music.
 *
 * No Arduino or ESP-IDF dependencies (host-tested: firmware/esp32_classic_bt_audio/test/host). Not thread-safe: the
 * caller serialises play / stop / mix (call_audio.cpp holds a mutex).
 */

#include <stddef.h>
#include <stdint.h>

static const uint16_t kChimeSoft = 2, kChimeCaution = 3, kChimeWarning = 4;
static const uint16_t kChimeAllOfNode = 0xFFFF;  // STOP: every sound that node started

class ChimeSynth {
 public:
  static const int kVoices = 4;
  static const int kMaxStrikes = 6;

  /** A sound this synth can play. */
  static bool known(uint16_t sound);
  /** One play of a sound, its trailing silence included (0 = unknown). */
  static uint32_t length_ms(uint16_t sound);

  /** Starts a sound (or restarts it: one voice per node and sound). priority 0-4, level 0-100 %, repeat 0 = until
   *  stop. False for a sound this synth does not have. */
  bool play(uint8_t node, uint16_t sound, uint8_t priority, uint8_t level_pct, uint8_t repeat);
  /** Stops a node's sound (kChimeAllOfNode = all of them). Stopped sounds are reported done. */
  void stop(uint8_t node, uint16_t sound);
  void stop_all();
  /** The chime volume, 1..15 on the master volume's curve: a setting of its own (the audio card's menu page), not
   *  the music's volume, so turning the music down never quietens an alert. 0 counts as 1: chimes are never silent. */
  void set_volume(uint8_t v_1_15) { _volume = v_1_15 > 15 ? 15 : (v_1_15 < 1 ? 1 : v_1_15); }

  /** Adds the chimes to `frames` frames of interleaved 16-bit PCM (1 or 2 channels) at `rate` Hz. With duck, the
   *  audio already in the buffer is turned down while an alert chime sounds (and back up after, with a short ramp). */
  void mix(int16_t *pcm, size_t frames, uint8_t channels, uint32_t rate, bool duck);

  bool active() const;
  /** Still changing the output: a sound plays, or the music is coming back up after a duck. */
  bool busy() const { return _duck < 1.0f || active(); }
  uint8_t count() const;
  /** A sound that finished (played out, stopped or replaced), for AUDIO_CMD DONE. False when there is none. */
  bool pop_done(uint8_t &node, uint16_t &sound);

 private:
  struct Strike {
    bool on = false;
    float env = 0, k = 0;      // envelope and its per-sample decay
    float a0 = 0, att = 0;     // attack: from a0 (a strike still ringing) to 1
    uint32_t n = 0;            // samples since the strike
    uint32_t phase[3] = {0, 0, 0};
    uint32_t inc[3] = {0, 0, 0};
  };
  struct Voice {
    bool on = false;
    uint8_t node = 0, prio = 0, level = 100, left = 1;  // left: plays still to go (0 = until stop)
    uint16_t sound = 0;
    const void *def = nullptr;
    uint32_t rate = 0;
    uint32_t t = 0;            // samples since this play began
    uint8_t next = 0;          // next strike of the sound to start
    uint32_t fade_len = 0, fade_left = 0;  // stopping: a short fade, then done
    float audible = 1.0f;      // 0 while a higher priority plays (ramped)
    Strike s[kMaxStrikes];
  };
  void finish(Voice &v);
  void retime(Voice &v, uint32_t rate);
  float sample(Voice &v);
  float gain(const Voice &v) const;  // per-voice output scale (chime volume, level)

  Voice _v[kVoices];
  uint8_t _volume = 8;
  float _duck = 1.0f;
  struct Done {
    uint8_t node;
    uint16_t sound;
  };
  Done _done[8];
  uint8_t _done_n = 0;
};
