#include "ai_audio.h"

#include <math.h>

namespace aiaudio {

static uint32_t gcd_u(uint32_t a, uint32_t b) {
  while (b) { const uint32_t t = a % b; a = b; b = t; }
  return a;
}

Resampler::Resampler(uint32_t in_rate, uint32_t out_rate, int taps) : _in(in_rate), _out(out_rate), _taps(taps & ~1) {
  // Downsampling: the transition band is a fraction of the (higher) input rate, so the kernel needs proportionally more taps.
  if (in_rate > out_rate) _taps = (taps * (int)((in_rate + out_rate - 1) / out_rate)) & ~1;
  const uint32_t g = gcd_u(in_rate, out_rate);
  _l = out_rate / g;
  _m = in_rate / g;
  // Cutoff in cycles per input sample: 0.46 of the lower of the two rates, relative to the input rate.
  const double lower = in_rate < out_rate ? in_rate : out_rate;
  const double fc = 0.46 * lower / in_rate;
  _h.resize((size_t)_l * _taps);
  const double half = _taps / 2.0;
  for (uint32_t ph = 0; ph < _l; ph++) {
    const double frac = (double)ph / _l;
    double sum = 0;
    for (int j = 0; j < _taps; j++) {
      // tap j reads input base + j - taps/2 + 1: distance from the output instant
      const double u = frac - (j - _taps / 2 + 1);
      const double a = 2 * fc * u;
      const double s = fabs(a) < 1e-9 ? 1.0 : sin(M_PI * a) / (M_PI * a);
      const double x = u / half;
      const double w = fabs(x) >= 1 ? 0 : 0.42 + 0.5 * cos(M_PI * x) + 0.08 * cos(2 * M_PI * x);  // Blackman
      const double g2 = 2 * fc * s * w;
      _h[(size_t)ph * _taps + j] = (float)g2;
      sum += g2;
    }
    for (int j = 0; j < _taps; j++) _h[(size_t)ph * _taps + j] = (float)(_h[(size_t)ph * _taps + j] / sum);  // unity DC gain per phase
  }
  reset();
}

void Resampler::reset() {
  _buf.assign(_taps, 0.0f);  // history of silence before the first sample
  _buf_first = -_taps;
  _k = 0;
}

void Resampler::process(const int16_t *in, size_t n, std::vector<int16_t> &out) {
  out.clear();
  for (size_t i = 0; i < n; i++) _buf.push_back((float)in[i]);
  const int64_t end = _buf_first + (int64_t)_buf.size();  // one past the last known input index
  for (;;) {
    const int64_t pos = _k * _m;                  // output instant, in units of 1/_l input samples
    const int64_t base = pos / _l;                // input index at or just before it
    const uint32_t ph = (uint32_t)(pos % _l);
    if (base + _taps / 2 >= end) break;           // needs input not here yet
    const float *h = &_h[(size_t)ph * _taps];
    float acc = 0;
    const int64_t first = base - _taps / 2 + 1;
    for (int j = 0; j < _taps; j++) acc += h[j] * _buf[(size_t)(first + j - _buf_first)];
    const long v = lrintf(acc);
    out.push_back((int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
    _k++;
  }
  // drop history that no later output needs
  const int64_t need = ((_k * (int64_t)_m) / _l) - _taps / 2 + 1;
  if (need - _buf_first > 4096) {
    const size_t drop = (size_t)(need - _buf_first);
    _buf.erase(_buf.begin(), _buf.begin() + drop);
    _buf_first += drop;
  }
}

void rail_left_to_s16(const int32_t *frames, size_t n_frames, int16_t *out) {
  for (size_t i = 0; i < n_frames; i++) out[i] = (int16_t)(frames[2 * i] >> 16);
}

void s16_to_rail_stereo(const int16_t *in, size_t n, int32_t *out_frames, int gain_pct) {
  if (gain_pct < 0) gain_pct = 0;
  if (gain_pct > 100) gain_pct = 100;
  for (size_t i = 0; i < n; i++) {
    const int32_t v = (int32_t)((int64_t)in[i] * gain_pct / 100) * 65536;
    out_frames[2 * i] = v;
    out_frames[2 * i + 1] = v;
  }
}

int level_dbfs10(const int16_t *x, size_t n) {
  if (!n) return -1000;
  double s = 0;
  for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
  const double rms = sqrt(s / n);
  if (rms < 1.0) return -1000;
  return (int)lrint(200.0 * log10(rms / 32768.0));
}

uint16_t SpeechPacer::credit() const {
  const uint64_t cap = (uint64_t)_cap_ms * _rate / 1000;
  if (_queued_samples >= cap) return 0;
  uint64_t bytes = (cap - _queued_samples) * 2;
  if (bytes > 0xFFFE) bytes = 0xFFFE;
  return (uint16_t)(bytes & ~1ull);
}

}  // namespace aiaudio
