#include "audio_dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace dsp {

namespace {

constexpr float kQ = 1.41f;            // one octave between the -3 dB points: the bands overlap smoothly
constexpr float kBandLimit = 0.45f;    // a band's centre must sit below this fraction of the rate
constexpr int kPregainPoints = 96;     // where the curve's peak is looked for, log-spaced 20 Hz .. 20 kHz

void *default_alloc(size_t n) { return malloc(n); }

/** Zeroth-order modified Bessel function (Kaiser window). */
double bessel_i0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 30; k++) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return sum;
}

/** Kaiser window at t in -1..1. */
double kaiser(double t, double beta) {
  if (t <= -1 || t >= 1) return 0;
  return bessel_i0(beta * sqrt(1 - t * t)) / bessel_i0(beta);
}

double sinc(double x) { return fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x); }

}  // namespace

void *(*alloc)(size_t bytes) = default_alloc;

// ---- helpers ----

float db_gain(float db) { return powf(10.0f, db / 20.0f); }

float volume_gain(uint8_t v_0_15) {
  if (!v_0_15) return 0.0f;
  if (v_0_15 > 15) v_0_15 = 15;
  const float zero = powf(1.4f, -12.0f);
  return (powf(1.4f, 12.0f * v_0_15 / 15.0f - 12.0f) - zero) / (1.0f - zero);
}

void fader_balance(int fader, int balance, float out[4]) {
  // n steps toward the far end: -2 dB a step, and the last one silences the side it leaves.
  auto cut = [](int n) { return n <= 0 ? 1.0f : (n >= 10 ? 0.0f : db_gain(-2.0f * (float)n)); };
  const float front = cut(-fader), rear = cut(fader), left = cut(balance), right = cut(-balance);
  out[0] = front * left;
  out[1] = front * right;
  out[2] = rear * left;
  out[3] = rear * right;
}

// ---- equaliser ----

const float Eq10::kCentreHz[kBands] = {31.25f, 62.5f, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

const int8_t Eq10::kPresetDb[kPresets][kBands] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},          // Flat
    {6, 6, 5, 3, 1, 0, 0, 0, 0, 0},          // Bass
    {0, 0, 0, 0, 0, 1, 2, 4, 5, 6},          // Treble
    {-3, -3, -2, 0, 2, 4, 4, 3, 1, 0},       // Vocal
    {5, 4, 2, -1, -2, -1, 1, 3, 4, 4},       // Rock
    {3, 2, 1, 2, -1, -1, 0, 1, 2, 3},        // Jazz
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},          // Custom: the user's own, kept by the engine's owner
};

void Eq10::set_rate(uint32_t rate) {
  if (rate == _d.rate || !rate) return;
  load(design(_d.db, rate));
  reset();
}

void Eq10::set_gains(const int8_t db[kBands]) { load(design(db, _d.rate)); }

void Eq10::load(const Design &d) {
  for (int b = 0; b < kBands; b++) {
    if (_d.db[b] == 0) _s[0][b] = _s[1][b] = State();
  }
  _d = d;
}

Eq10::Design Eq10::design(const int8_t db[kBands], uint32_t rate) {
  Design d;
  d.rate = rate ? rate : 48000;
  for (int b = 0; b < kBands; b++) {
    const int v = db[b];
    d.db[b] = (int8_t)(v > kMaxDb ? kMaxDb : (v < -kMaxDb ? -kMaxDb : v));
    Coef &c = d.c[b];
    if (d.db[b] == 0 || kCentreHz[b] >= kBandLimit * (float)d.rate) continue;
    // RBJ peaking EQ, in double: the low bands' poles sit close to 1.
    const double A = pow(10.0, d.db[b] / 40.0);
    const double w0 = 2 * M_PI * kCentreHz[b] / d.rate;
    const double alpha = sin(w0) / (2 * kQ);
    const double cw = cos(w0);
    const double a0 = 1 + alpha / A;
    c.b0 = (float)((1 + alpha * A) / a0);
    c.b1 = (float)((-2 * cw) / a0);
    c.b2 = (float)((1 - alpha * A) / a0);
    c.a1 = (float)((-2 * cw) / a0);
    c.a2 = (float)((1 - alpha / A) / a0);
    d.idx[d.active++] = (uint8_t)b;
  }
  // The pre-gain: the inverse of the curve's highest point (only when it rises above 0 dB).
  float peak = 0;
  if (d.active) {
    const float top = 20000.0f < 0.48f * d.rate ? 20000.0f : 0.48f * d.rate;
    for (int i = 0; i < kPregainPoints; i++) {
      const float hz = 20.0f * powf(top / 20.0f, (float)i / (kPregainPoints - 1));
      const float r = response_db(d, hz);
      if (r > peak) peak = r;
    }
    for (int b = 0; b < d.active; b++) {  // and at the centres themselves
      const float r = response_db(d, kCentreHz[d.idx[b]]);
      if (r > peak) peak = r;
    }
  }
  d.pre = db_gain(-peak);
  return d;
}

float Eq10::response_db(const Design &d, float hz) {
  const double w = 2 * M_PI * hz / d.rate;
  const double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
  double db = 0;
  for (int i = 0; i < d.active; i++) {
    const Coef &c = d.c[d.idx[i]];
    // |H(e^jw)| = |b0 + b1 z^-1 + b2 z^-2| / |1 + a1 z^-1 + a2 z^-2|
    const double nr = c.b0 + c.b1 * c1 + c.b2 * c2, ni = -(c.b1 * s1 + c.b2 * s2);
    const double dr = 1 + c.a1 * c1 + c.a2 * c2, di = -(c.a1 * s1 + c.a2 * s2);
    db += 10 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
  }
  return (float)db;
}

void Eq10::process(float *l, float *r, size_t n) {
  if (!_d.active) return;
  float *ch[2] = {l, r};
  for (int k = 0; k < 2; k++) {
    float *x = ch[k];
    if (_d.pre != 1.0f) {
      for (size_t i = 0; i < n; i++) x[i] *= _d.pre;
    }
    for (int j = 0; j < _d.active; j++) {
      const int b = _d.idx[j];
      const Coef c = _d.c[b];
      State s = _s[k][b];
      for (size_t i = 0; i < n; i++) {  // direct form I: the quietest for low bands in float
        const float in = x[i];
        const float y = c.b0 * in + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
        s.x2 = s.x1;
        s.x1 = in;
        s.y2 = s.y1;
        s.y1 = y;
        x[i] = y;
      }
      // flush denormals: a band ringing out into silence would otherwise crawl
      if (fabsf(s.y1) < 1e-20f) s.y1 = 0;
      if (fabsf(s.y2) < 1e-20f) s.y2 = 0;
      _s[k][b] = s;
    }
  }
}

void Eq10::reset() {
  for (auto &ch : _s) {
    for (auto &s : ch) s = State();
  }
}

// ---- resampler ----

bool Resampler::configure(uint32_t in_rate, uint32_t out_rate, uint8_t channels) {
  if (!in_rate || !out_rate) return false;
  _ch = channels == 1 ? 1 : 2;
  _in = in_rate;
  _out = out_rate;
  _ratio = (double)in_rate / out_rate;
  set_trim(1.0);
  // Cutoff, as a fraction of the input rate: just under the narrower of the two bands. A 60 dB Kaiser window over 32
  // taps leaves a transition of about 0.11 of the input rate, centred here, so 44.1 kHz passes to about 19 kHz and its
  // images (above 24.1 kHz) are down by 60 dB.
  const double band = in_rate <= out_rate ? 0.5 : 0.5 * out_rate / in_rate;
  const float cut = (float)(band * 0.92);
  if (!_table) _table = static_cast<float *>(alloc(sizeof(float) * (kPhases + 1) * kTaps));
  if (!_table) return false;
  if (cut != _table_cut) {
    const double beta = 5.65;  // 60 dB
    for (int p = 0; p <= kPhases; p++) {
      float *row = _table + p * kTaps;
      double sum = 0;
      for (int k = 0; k < kTaps; k++) {
        // tap k weighs input frame (i - kTaps/2 + 1 + k) for an output at i + p / kPhases
        const double d = (double)(k - kTaps / 2 + 1) - (double)p / kPhases;
        const double h = 2 * cut * sinc(2 * cut * d) * kaiser(d / (kTaps / 2), beta);
        row[k] = (float)h;
        sum += h;
      }
      for (int k = 0; k < kTaps; k++) row[k] = (float)(row[k] / sum);  // unity gain at DC in every phase
    }
    _table_cut = cut;
  }
  reset();
  return true;
}

void Resampler::set_trim(double trim) {
  _trim = trim;
  _step = _ratio * trim;
}

void Resampler::reset() {
  memset(_hist, 0, sizeof(_hist));
  _count = kTaps;            // start with a history of silence, so the first output frame has its taps
  _pos = kTaps / 2 - 1;      // the newest input sits at the right edge of the window
}

bool Resampler::refill(PullFn pull, void *ctx) {
  // Keep the frames the next output still needs (its window starts kTaps/2 - 1 before _pos), drop the rest.
  int keep_from = (int)_pos - (kTaps / 2 - 1);
  if (keep_from < 0) keep_from = 0;
  if (keep_from > _count) keep_from = _count;
  const int keep = _count - keep_from;
  for (int c = 0; c < _ch; c++) memmove(_hist[c], _hist[c] + keep_from, sizeof(float) * keep);
  _count = keep;
  _pos -= keep_from;
  int room = kTaps + kChunk - _count;
  if (room > kChunk) room = kChunk;  // _pull holds kChunk frames
  const size_t got = room > 0 ? pull(_pull, (size_t)room, ctx) : 0;
  for (size_t i = 0; i < got; i++) {
    for (int c = 0; c < _ch; c++) _hist[c][_count + (int)i] = _pull[i * _ch + c] * (1.0f / 32768.0f);
  }
  _count += (int)got;
  return got > 0;
}

size_t Resampler::process(float *l, float *r, size_t n, PullFn pull, void *ctx) {
  if (!_table) {
    for (size_t i = 0; i < n; i++) l[i] = r[i] = 0;
    return 0;
  }
  float coef[kTaps];
  size_t made = 0;
  for (; made < n; made++) {
    int ip = (int)_pos;
    if (ip + kTaps / 2 >= _count) {
      if (!refill(pull, ctx)) break;
      ip = (int)_pos;
      if (ip + kTaps / 2 >= _count) break;
    }
    const double frac = _pos - ip;
    const float ph = (float)(frac * kPhases);
    int p0 = (int)ph;
    float t = ph - (float)p0;
    if (p0 >= kPhases) {  // frac a hair under 1, rounded up in float
      p0 = kPhases - 1;
      t = 1;
    }
    const float *a = _table + p0 * kTaps, *b = a + kTaps;
    for (int k = 0; k < kTaps; k++) coef[k] = a[k] + t * (b[k] - a[k]);
    const int first = ip - kTaps / 2 + 1;
    float acc0 = 0, acc1 = 0;
    const float *x0 = _hist[0] + first;
    for (int k = 0; k < kTaps; k++) acc0 += coef[k] * x0[k];
    if (_ch == 2) {
      const float *x1 = _hist[1] + first;
      for (int k = 0; k < kTaps; k++) acc1 += coef[k] * x1[k];
    } else {
      acc1 = acc0;
    }
    l[made] = acc0;
    if (r != l) r[made] = acc1;
    _pos += _step;
  }
  for (size_t i = made; i < n; i++) {
    l[i] = 0;
    if (r != l) r[i] = 0;
  }
  return made;
}

// ---- decimator ----

void Decimator::configure(uint32_t in_rate, int factor) {
  (void)in_rate;
  _m = factor < 1 ? 1 : (factor > 6 ? 6 : factor);
  // Low-pass centred at 0.42 of the output rate (Kaiser, 60 dB, 96 taps: about 1.8 kHz wide at 48 kHz), so it is
  // down by 60 dB before the new Nyquist and nothing folds back; unity at DC. Voice needs no more than 0.4 of it.
  const double cut = 0.42 / _m;  // of the input rate
  double sum = 0;
  for (int k = 0; k < kTaps; k++) {
    const double d = k - (kTaps - 1) / 2.0;
    const double h = 2 * cut * sinc(2 * cut * d) * kaiser(d / (kTaps / 2.0), 5.65);
    _h[k] = (float)h;
    sum += h;
  }
  for (float &h : _h) h = (float)(h / sum);
  reset();
}

void Decimator::reset() {
  memset(_x, 0, sizeof(_x));
  _w = 0;
  _phase = 0;
}

size_t Decimator::process(const float *in, size_t n, float *out) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    _x[_w] = in[i];
    _w = (_w + 1) % kTaps;
    if (++_phase < _m) continue;
    _phase = 0;
    float acc = 0;
    int j = _w;  // the oldest sample
    for (int k = 0; k < kTaps; k++) {
      acc += _h[k] * _x[j];
      j = j + 1 == kTaps ? 0 : j + 1;
    }
    out[o++] = acc;
  }
  return o;
}

// ---- ring ----

bool FrameRing::init(size_t frames, uint8_t channels) {
  _ch = channels == 1 ? 1 : 2;
  _cap = frames + 1;
  _buf = static_cast<int16_t *>(alloc(sizeof(int16_t) * _cap * _ch));
  _w = 0;
  _r = 0;
  return _buf != nullptr;
}

size_t FrameRing::level() const {
  const size_t w = _w.load(std::memory_order_acquire), r = _r.load(std::memory_order_acquire);
  return (w + _cap - r) % (_cap ? _cap : 1);
}

size_t FrameRing::write(const int16_t *frames, size_t n) {
  if (!_buf) return 0;
  size_t w = _w.load(std::memory_order_relaxed);
  const size_t r = _r.load(std::memory_order_acquire);
  const size_t free_frames = (r + _cap - w - 1) % _cap;
  if (n > free_frames) n = free_frames;
  for (size_t i = 0; i < n; i++) {
    memcpy(_buf + w * _ch, frames + i * _ch, sizeof(int16_t) * _ch);
    w = w + 1 == _cap ? 0 : w + 1;
  }
  _w.store(w, std::memory_order_release);
  return n;
}

size_t FrameRing::read(int16_t *frames, size_t n) {
  if (!_buf) return 0;
  size_t r = _r.load(std::memory_order_relaxed);
  const size_t w = _w.load(std::memory_order_acquire);
  const size_t have = (w + _cap - r) % _cap;
  if (n > have) n = have;
  for (size_t i = 0; i < n; i++) {
    memcpy(frames + i * _ch, _buf + r * _ch, sizeof(int16_t) * _ch);
    r = r + 1 == _cap ? 0 : r + 1;
  }
  _r.store(r, std::memory_order_release);
  return n;
}

void FrameRing::clear() { _r.store(_w.load(std::memory_order_acquire), std::memory_order_release); }

}  // namespace dsp
