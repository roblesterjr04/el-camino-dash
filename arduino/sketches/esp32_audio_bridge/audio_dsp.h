#pragma once

/*
 * The audio card's signal processing, used by the output engine (audio_engine.*):
 *
 *   Eq10        the 10-band graphic equaliser: peaking biquads (RBJ cookbook, Q 1.41 = one octave) at the ISO octave
 *               centres 31 Hz .. 16 kHz, -12..+12 dB, with a pre-gain that keeps the boosted curve from clipping
 *   Resampler   a windowed-sinc interpolator (32 taps, 64 phases with linear interpolation between them) that turns
 *               any rate at or below the output's into it, with a trim for the clock drift of a stream it is fed
 *               from (a phone's A2DP, the call's voice)
 *   Decimator   48 kHz to 16 or 8 kHz for the microphone (a low-pass FIR, then one sample in M)
 *   FrameRing   a single-producer single-consumer ring of 16-bit frames between a Bluetooth task and the engine
 *
 * No Arduino or ESP-IDF dependencies: host-tested (firmware/esp32_classic_bt_audio/test/host). Not thread-safe except
 * FrameRing (one writer, one reader).
 */

#include <stddef.h>
#include <stdint.h>

#include <atomic>

namespace dsp {

/** Memory for the tables and rings. The engine points it at internal RAM first (the tables are read every sample);
 *  the host tests leave it at malloc. Never freed: everything is made once at boot. */
extern void *(*alloc)(size_t bytes);

// ---- equaliser ----

class Eq10 {
 public:
  static const int kBands = 10;
  static const int kMaxDb = 12;
  static const float kCentreHz[kBands];
  static const int kPresets = 7;  // ELC_EQ_PRESETS: Flat, Bass, Treble, Vocal, Rock, Jazz, Custom
  /** The fixed presets' curves in dB (Custom's row is unused: it is the user's own). */
  static const int8_t kPresetDb[kPresets][kBands];

  struct Coef {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  };
  /** A curve's coefficients and pre-gain, worked out apart from the filtering (double maths: slow on the ESP32, so the
   *  engine's caller does it and the audio task only loads it). */
  struct Design {
    uint32_t rate = 48000;
    int8_t db[kBands] = {0};
    Coef c[kBands];
    uint8_t idx[kBands] = {0};  // the bands that are not flat, in order
    int active = 0;
    float pre = 1.0f;
  };
  /** Band gains in dB at a rate (clamped to +-kMaxDb; bands at or above 0.45 of the rate are left flat). The
   *  pre-gain is the inverse of the curve's highest point when it rises above 0 dB, so a boosted band cannot clip. */
  static Design design(const int8_t db[kBands], uint32_t rate);
  /** Puts a design in use. The filter state of a band that was flat starts from silence. */
  void load(const Design &d);

  /** The sample rate (coefficients follow it). */
  void set_rate(uint32_t rate);
  /** design() at the current rate and load(). */
  void set_gains(const int8_t db[kBands]);
  const int8_t *gains() const { return _d.db; }
  bool flat() const { return _d.active == 0; }
  float pregain() const { return _d.pre; }
  /** The bands' combined magnitude at hz, in dB, without the pre-gain (for the tests and the bench). */
  float response_db(float hz) const { return response_db(_d, hz); }
  static float response_db(const Design &d, float hz);

  /** Filters n frames of planar stereo in place (pre-gain included). Flat bands cost nothing. */
  void process(float *l, float *r, size_t n);
  /** Forgets the filter state (a new source starts from silence). */
  void reset();

 private:
  struct State {
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  };
  Design _d;
  State _s[2][kBands];
};

// ---- resampler ----

/** Where a Resampler pulls its input from: up to n frames of interleaved 16-bit audio (1 or 2 channels) into out,
 *  returning how many it had. */
using PullFn = size_t (*)(int16_t *out, size_t frames, void *ctx);

class Resampler {
 public:
  static const int kTaps = 32;
  static const int kPhases = 64;

  /** in_rate (<= out_rate; a higher one is cut to out_rate's band) to out_rate, `channels` 1 or 2. Builds the table
   *  (once per rate pair) and forgets the history. False when the table could not be allocated. */
  bool configure(uint32_t in_rate, uint32_t out_rate, uint8_t channels);
  uint32_t in_rate() const { return _in; }
  /** Input frames consumed per output frame = in / out * trim. trim near 1 (the engine's drift control). */
  void set_trim(double trim);
  double trim() const { return _trim; }

  /** n output frames into planar l / r (r may equal l for mono, which writes the one channel). Pulls input as it
   *  needs it; when the input runs dry, the rest is silence and the result is the frames made (the caller counts an
   *  underrun). */
  size_t process(float *l, float *r, size_t n, PullFn pull, void *ctx);
  /** Forgets the history (a stream that stopped). */
  void reset();

 private:
  static const int kChunk = 128;  // input frames pulled at a time
  bool refill(PullFn pull, void *ctx);

  uint32_t _in = 0, _out = 0;
  uint8_t _ch = 2;
  double _ratio = 1, _trim = 1, _step = 1;
  float *_table = nullptr;  // (kPhases + 1) x kTaps
  float _table_cut = 0;     // the cutoff it was built for
  float _hist[2][kTaps + kChunk] = {{0}};
  int _count = 0;   // frames in _hist
  double _pos = 0;  // read position in _hist (input frames)
  int16_t _pull[kChunk * 2] = {0};
};

// ---- decimator ----

class Decimator {
 public:
  static const int kTaps = 96;
  /** in_rate to in_rate / factor (factor 1..6). */
  void configure(uint32_t in_rate, int factor);
  int factor() const { return _m; }
  /** Feeds n input samples; writes one output per `factor` inputs to out and returns how many. */
  size_t process(const float *in, size_t n, float *out);
  void reset();

 private:
  int _m = 1;
  float _h[kTaps] = {0};
  float _x[kTaps] = {0};
  int _w = 0;      // next write in _x
  int _phase = 0;  // inputs since the last output
};

// ---- ring ----

class FrameRing {
 public:
  /** frames of `channels` 16-bit samples. False when it could not be allocated. */
  bool init(size_t frames, uint8_t channels);
  size_t capacity() const { return _cap - 1; }
  uint8_t channels() const { return _ch; }
  /** Producer side. Returns the frames written (fewer when full). */
  size_t write(const int16_t *frames, size_t n);
  /** Consumer side. */
  size_t read(int16_t *frames, size_t n);
  size_t level() const;
  size_t space() const { return capacity() - level(); }
  /** Consumer side: drops everything queued. */
  void clear();

 private:
  int16_t *_buf = nullptr;
  size_t _cap = 0;
  uint8_t _ch = 2;
  std::atomic<size_t> _w{0}, _r{0};
};

// ---- helpers ----

/** dB to a linear gain. */
float db_gain(float db);
/** The master volume's curve, 0..15 (the chimes use the same one): 1.4^x over 12 bits, 0 = silent. */
float volume_gain(uint8_t v_0_15);
/** Front / rear and left / right gains from fader (-10 rear .. +10 front) and balance (-10 left .. +10 right):
 *  out[0] FL, [1] FR, [2] RL, [3] RR. Centre = 1 everywhere; the far end of either is silent. */
void fader_balance(int fader, int balance, float out[4]);

}  // namespace dsp
