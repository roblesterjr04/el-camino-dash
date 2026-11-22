#pragma once

// Audio helpers for the AI card (no Arduino dependencies, host-tested): the sample-rate converter between the rail
// (44.1 / 48 kHz, 32-bit slots, stereo) and the speech models (16 kHz in, 16-24 kHz out), format conversion, the
// level meter and the pacing of the module's speech.

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace aiaudio {

/** Streaming polyphase windowed-sinc resampler, mono. Rates in Hz; any pair (reduced by their gcd). The anti-alias /
 *  anti-image cutoff is 0.46 of the lower rate. Latency is taps / 2 input samples. */
class Resampler {
 public:
  Resampler(uint32_t in_rate, uint32_t out_rate, int taps = 32);
  /** Consumes n input samples and appends the output samples that can be computed to `out` (cleared first). */
  void process(const int16_t *in, size_t n, std::vector<int16_t> &out);
  void reset();
  uint32_t inRate() const { return _in; }
  uint32_t outRate() const { return _out; }

 private:
  uint32_t _in, _out;
  uint32_t _l, _m;  // output rate / gcd, input rate / gcd
  int _taps;
  std::vector<float> _h;      // _l phases x _taps
  std::vector<float> _buf;    // input history
  int64_t _buf_first = 0;     // absolute index of _buf[0]
  int64_t _k = 0;             // next output sample number
};

/** The microphone on the rail: the left channel of 32-bit stereo frames (24-bit data, left-justified) to 16-bit. */
void rail_left_to_s16(const int32_t *frames, size_t n_frames, int16_t *out);
/** Speech to the rail: mono 16-bit to 32-bit stereo (same sample on both channels), scaled by gain_pct (0..100). */
void s16_to_rail_stereo(const int16_t *in, size_t n, int32_t *out_frames, int gain_pct = 100);

/** Level of a block in dBFS x 10 (-1000 for silence). */
int level_dbfs10(const int16_t *x, size_t n);

/** How much speech the module may send. The ESP32 keeps about capacity_ms of speech queued at most; the module is told
 *  how many bytes (at its own rate) it may send now. */
class SpeechPacer {
 public:
  explicit SpeechPacer(uint32_t capacity_ms = 1500) : _cap_ms(capacity_ms) {}
  void reset(uint32_t rate_hz) { _rate = rate_hz; _queued_samples = 0; }
  void setRate(uint32_t rate_hz) { _rate = rate_hz; }
  void received(size_t bytes) { _queued_samples += bytes / 2; }
  void played_ms(uint32_t ms) {
    const uint64_t s = (uint64_t)ms * _rate / 1000;
    _queued_samples = s >= _queued_samples ? 0 : _queued_samples - s;
  }
  uint32_t queued_ms() const { return _rate ? (uint32_t)((uint64_t)_queued_samples * 1000 / _rate) : 0; }
  /** Bytes that may be sent now (even, at most 0xFFFF). */
  uint16_t credit() const;

 private:
  uint32_t _cap_ms, _rate = 22050;
  uint64_t _queued_samples = 0;
};

}  // namespace aiaudio
