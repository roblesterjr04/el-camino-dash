#pragma once

/*
 * The audio card's output engine: one task that runs the I2S all the time at 48 kHz (32-bit slots, so BCK = 64 fS
 * and MCLK = 256 fS = 12.288 MHz, which the DACs, the ADC and the rail's cards need) and makes every block of output:
 *
 *   music     the source the sink plays (sink.cpp): Bluetooth (A2DP, resampled from the phone's rate with its clock
 *             drift trimmed out), the radio (the rail's RET, already at 48 kHz on the card's own clock) or nothing. Through the 10-band equaliser, then the master volume and mute.
 *   voice     a call's audio (HFP: 8 or 16 kHz, resampled), or a voice card talking on RET: over the music, which
 *             ducks under a voice card. Not equalised.
 *   chimes    the alert chimes (chimes.h), at their own volume, ducking everything under an alert.
 *
 * then fader and balance, to the front DAC (I2S0, master) and the rear DAC (I2S1, a slave on the same BCK / WS, so
 * the two never drift). In the same pass it reads the ADC (I2S0 data in: the microphone on the left; there is no
 * aux in) and the rail's RET (I2S1 data in), and during a call sends the microphone, decimated to the call's rate, to
 * the phone. The rail buffer (the rail's clocks, MIC and MCLK out to the other slots) goes on once the clocks run.
 *
 * Threading: the engine task (core 1) does all the I2S and DSP work. The setters are called from the loop or from
 * Bluetooth tasks and only post values the task picks up at its next block (5 ms). A2DP writes into a ring
 * (A2dpInStream); the call's voice into another (call_audio_rx); the stack pulls the microphone (call_audio_tx).
 */

#include <stddef.h>
#include <stdint.h>

#include "AudioTools.h"

struct EnginePins {
  int bck = -1, ws = -1, dout = -1;  // I2S0 master: the front DAC (or the DevKit's DAC / amplifier)
  int din = -1;                      // I2S0 data in: the microphone (the ADC's left channel); -1 = none
  int mclk = -1;                     // the ADC's (and the rail's) master clock: GPIO0, 1 or 3; -1 = none
  int rear_dout = -1;                // I2S1 data out (slave on bck / ws): the rear DAC; -1 = none
  int ret_din = -1;                  // I2S1 data in: the rail's RET; -1 = none
  int rail_en = -1;                  // the rail buffer's enable, low = on; -1 = none
  bool mic_right = false;            // microphone on the ADC's right channel instead
};

enum class Music : uint8_t { Off, Bluetooth, Rail };

/** Once, before A2DP starts. False when the I2S could not be set up (no audio at all). */
bool engine_begin(const EnginePins &pins);
bool engine_has_rear();  // the rear DAC is fitted (fader means something)
bool engine_has_rail();  // RET can be heard

/** What the music is. Bluetooth's ring is drained while it is not the music, so a phone that keeps streaming never
 *  blocks. */
void engine_set_music(Music m);
/** The card holding RET is talking (a voice card, not the radio): its audio goes over the music, which ducks. */
void engine_set_rail_voice(bool on);
/** The master volume, 0..15 (dsp::volume_gain's curve), and mute: the music and a voice card, not calls or chimes. */
void engine_set_volume(uint8_t v_0_15);
void engine_set_mute(bool muted);
/** Band gains in dB, -12..12 (dsp::Eq10). */
void engine_set_eq(const int8_t db[10]);
/** -10 (rear) .. 10 (front), -10 (left) .. 10 (right). */
void engine_set_fader_balance(int8_t fader, int8_t balance);
/** Bluetooth audio is coming in (the phone streams) / was heard in the last 300 ms. */
bool engine_bt_flowing();
/** One line for the serial console: CPU per block, Bluetooth ring and trim, underruns, RET and ADC levels. */
void engine_status(char *out, size_t n);

/** A2DP's output: into the engine's Bluetooth ring at the phone's rate. */
class A2dpInStream : public audio_tools::AudioStream {
 public:
  bool begin() override;
  void end() override;
  void setAudioInfo(audio_tools::AudioInfo info) override;
  audio_tools::AudioInfo audioInfo() override { return _info; }
  size_t write(const uint8_t *data, size_t len) override;
  int availableForWrite() override;

 private:
  audio_tools::AudioInfo _info{44100, 2, 16};
};

// ---- call audio (HFP), from phone.cpp ----

/** The call's audio link came up. rate = 8000 (CVSD) or 16000 (mSBC). */
void call_audio_start(uint32_t rate);
/** The call's audio link went down. */
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
/** An alert chime (priority >= 2) is ducking the music. */
bool chime_ducking();
