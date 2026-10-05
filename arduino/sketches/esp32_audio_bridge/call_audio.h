#pragma once

/*
 * Call audio for the hands-free profile (HFP), sharing I2S0 with A2DP.
 *
 * A2DP writes through CallGateStream, which forwards to the I2SStream until a call's audio (SCO) comes up. Then
 * the gate closes the I2SStream and a call task owns I2S0 for the length of the call: it plays the phone's voice
 * (8 kHz CVSD or 16 kHz mSBC, both decoded to 16-bit PCM by the stack) and records the microphone (the carrier's
 * PCM1808 on the I2S0 data-in pin) into a ring the stack pulls from. With the stack's own codec (this build), audio uses
 * the callback API (esp_hf_client_register_data_callback): the audio-buffer API is only for an external codec, where
 * the app would pass encoded mSBC. During a call I2S0 runs at 16 kHz with 32-bit slots, which
 * gives the 64 fS bit clock and 256 fS master clock the PCM1808 needs (it does not run below 16 kHz, so 8 kHz calls
 * are resampled). When the call audio ends, the gate reopens the I2SStream in A2DP's format if A2DP wants it.
 *
 * The alert chimes (chimes.h) share I2S0 the same way: A2DP's writes have them mixed in (and the music ducked under an
 * alert), the call task mixes them into the phone's voice, and when neither is writing a chime task opens I2S0 in
 * A2DP's last format, plays them out and closes it again.
 *
 * Threading: call_audio_start/stop/rx/tx are called from Bluetooth tasks; the call task does all I2S work. The chime_*
 * calls come from the loop.
 */

#include <stddef.h>
#include <stdint.h>

#include "AudioTools.h"

struct CallAudioPins {
  int bck = -1, ws = -1, dout = -1;
  int din = -1;   // microphone ADC data (-1 = no microphone: silence goes to the phone)
  int mclk = -1;  // master clock for the ADC (GPIO0, 1 or 3 on the ESP32; -1 = none)
  bool mic_right = false;  // microphone on the right channel of the ADC (left by default)
};

/** A2DP's output: forwards to the I2S stream except while a call owns I2S0 (audio written then is dropped). */
class CallGateStream : public audio_tools::AudioStream {
 public:
  explicit CallGateStream(audio_tools::I2SStream &i2s) : _i2s(i2s) {}
  bool begin() override;
  void end() override;
  void setAudioInfo(audio_tools::AudioInfo info) override;
  audio_tools::AudioInfo audioInfo() override { return _i2s.audioInfo(); }
  size_t write(const uint8_t *data, size_t len) override;
  int availableForWrite() override { return _i2s.availableForWrite(); }

 private:
  audio_tools::I2SStream &_i2s;
};

/** Once, before A2DP starts. */
void call_audio_begin(audio_tools::I2SStream *i2s, const CallAudioPins &pins);

/** The call's audio link came up: take I2S0 over. rate = 8000 (CVSD) or 16000 (mSBC). */
void call_audio_start(uint32_t rate);
/** The call's audio link went down: give I2S0 back to A2DP. */
void call_audio_stop();
/** Phone -> car PCM from the stack (16-bit mono at the call rate). */
void call_audio_rx(const uint8_t *pcm, size_t len, bool bad_frame);
/** Car -> phone: the stack pulls len bytes of PCM; returns len, or 0 when there is not a whole frame yet. */
uint32_t call_audio_tx(uint8_t *buf, uint32_t len);

void call_audio_set_mute(bool muted);
/** Speaker gain 0..15 (HFP scale; 0 = silent, 15 = full). */
void call_audio_set_volume(uint8_t vol_0_15);
bool call_audio_active();

/** One line for the serial console: rate, frames each way, underruns. */
void call_audio_status(char *out, size_t n);

// ---- chimes (AUDIO_CMD built-in sounds 2-4) ----
/** Starts a chime for `node` (priority 0-4, level %, repeat 0 = until stopped). False for a sound there is none of. */
bool chime_play(uint8_t node, uint16_t sound, uint8_t priority, uint8_t level_pct, uint8_t repeat);
/** Stops a node's chime (0xFFFF = all of that node's). */
void chime_stop(uint8_t node, uint16_t sound);
/** The chime volume, 1..15 (the audio card's setting; not the music's volume). */
void chime_set_volume(uint8_t v_1_15);
/** A chime that ended (for AUDIO_CMD DONE); false when there is none. */
bool chime_pop_done(uint8_t &node, uint16_t &sound);
uint8_t chime_count();
