// Host tests for the audio card's signal processing (audio_dsp.*): the equaliser's curve and pre-gain, the
// resampler's accuracy and image rejection, the microphone decimator, the frame ring and the fader / balance gains.

#include "audio_dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

static int s_fail = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      s_fail++;                                                        \
    }                                                                  \
  } while (0)
#define NEAR(a, b, tol)                                                                                     \
  do {                                                                                                      \
    const double _a = (a), _b = (b);                                                                        \
    if (fabs(_a - _b) > (tol)) {                                                                            \
      fprintf(stderr, "%s:%d: %s = %.4f, want %.4f +- %.4f\n", __FILE__, __LINE__, #a, _a, _b, (double)(tol)); \
      s_fail++;                                                                                             \
    }                                                                                                       \
  } while (0)

static const double kPi = 3.14159265358979;

/** Amplitude and residual of the best-fitting sine at hz (least squares on sin / cos), from sample `skip` on. */
static void fit_sine(const std::vector<float> &x, double rate, double hz, size_t skip, double *amp, double *resid_db) {
  double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
  for (size_t i = skip; i < x.size(); i++) {
    const double w = 2 * kPi * hz * i / rate, s = sin(w), c = cos(w);
    ss += s * s;
    cc += c * c;
    sc += s * c;
    xs += x[i] * s;
    xc += x[i] * c;
  }
  const double det = ss * cc - sc * sc;
  const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
  double e = 0, p = 0;
  for (size_t i = skip; i < x.size(); i++) {
    const double w = 2 * kPi * hz * i / rate;
    const double fit = a * sin(w) + b * cos(w);
    e += (x[i] - fit) * (x[i] - fit);
    p += fit * fit;
  }
  *amp = sqrt(a * a + b * b);
  *resid_db = 10 * log10((e + 1e-30) / (p + 1e-30));
}

static double rms(const std::vector<float> &x, size_t skip) {
  double s = 0;
  for (size_t i = skip; i < x.size(); i++) s += x[i] * x[i];
  return sqrt(s / (double)(x.size() - skip));
}

/** A sine at hz through the equaliser at 48 kHz; the output's level in dB against the input's. */
static double eq_gain_db(dsp::Eq10 &eq, double hz) {
  const size_t n = 48000;
  std::vector<float> l(n), r(n);
  for (size_t i = 0; i < n; i++) l[i] = r[i] = (float)(0.25 * sin(2 * kPi * hz * i / 48000));
  eq.reset();
  eq.process(l.data(), r.data(), n);
  std::vector<float> in(n);
  for (size_t i = 0; i < n; i++) in[i] = (float)(0.25 * sin(2 * kPi * hz * i / 48000));
  return 20 * log10(rms(l, n / 2) / rms(in, n / 2));
}

static void test_eq() {
  dsp::Eq10 eq;
  eq.set_rate(48000);
  int8_t g[10] = {0};
  eq.set_gains(g);
  CHECK(eq.flat());
  NEAR(eq.pregain(), 1.0, 1e-6);
  {
    float l[4] = {0.1f, -0.2f, 0.3f, 0.4f}, r[4] = {0.5f, 0.6f, -0.7f, 0.8f};
    eq.process(l, r, 4);
    NEAR(l[2], 0.3, 1e-7);  // flat: untouched
    NEAR(r[3], 0.8, 1e-7);
  }
  // One band at a time: the curve peaks at its centre with the gain asked for (the bands don't touch each other here).
  for (int b = 0; b < 10; b++) {
    for (int db : {6, -12, 12}) {
      memset(g, 0, sizeof(g));
      g[b] = (int8_t)db;
      eq.set_gains(g);
      NEAR(eq.response_db(dsp::Eq10::kCentreHz[b]), db, 0.05);
      // An octave and a half away the band has mostly let go.
      NEAR(eq.response_db(dsp::Eq10::kCentreHz[b] * (b < 9 ? 2.83f : 0.354f)), 0, fabs(db) * 0.25);
      // The pre-gain cancels the boost at its peak, and does nothing for a cut.
      NEAR(20 * log10(eq.pregain()), db > 0 ? -db : 0, 0.05);
    }
  }
  // Measured through the filters: +6 dB at 1 kHz comes out at 0 dB there (pre-gain) and -6 dB far from it.
  memset(g, 0, sizeof(g));
  g[5] = 6;
  eq.set_gains(g);
  NEAR(eq_gain_db(eq, 1000), 0.0, 0.15);
  NEAR(eq_gain_db(eq, 100), -6.0, 0.3);
  // A cut at 62 Hz, measured.
  memset(g, 0, sizeof(g));
  g[1] = -9;
  eq.set_gains(g);
  NEAR(eq_gain_db(eq, 62.5), -9.0, 0.3);
  NEAR(eq_gain_db(eq, 2000), 0.0, 0.1);
  // Every band at +12: the curve rises well above 12 dB where bands overlap, and the pre-gain covers all of it:
  // nothing anywhere comes out louder than it went in.
  for (auto &x : g) x = 12;
  eq.set_gains(g);
  CHECK(20 * log10(eq.pregain()) <= -12.0);
  for (double hz : {40.0, 90.0, 180.0, 700.0, 1400.0, 3000.0, 6000.0, 11000.0, 15000.0}) {
    CHECK(eq_gain_db(eq, hz) <= 0.1);
  }
  // Clamped to +-12.
  int8_t big[10] = {40, -40, 0, 0, 0, 0, 0, 0, 0, 0};
  eq.set_gains(big);
  CHECK(eq.gains()[0] == 12 && eq.gains()[1] == -12);
  // The presets stay inside the range, and Flat is flat.
  for (int p = 0; p < dsp::Eq10::kPresets; p++) {
    for (int b = 0; b < 10; b++) CHECK(abs(dsp::Eq10::kPresetDb[p][b]) <= 12);
  }
  eq.set_gains(dsp::Eq10::kPresetDb[0]);
  CHECK(eq.flat());
  // Bass: the low end comes up against the top.
  eq.set_gains(dsp::Eq10::kPresetDb[1]);
  CHECK(eq.response_db(50) > eq.response_db(5000) + 5);
  // At 16 kHz the band is still there at 48 kHz, and left flat at 32 kHz (above 0.45 of it).
  memset(g, 0, sizeof(g));
  g[9] = 6;
  eq.set_gains(g);
  NEAR(eq.response_db(16000), 6, 0.1);
  dsp::Eq10 low;
  low.set_rate(32000);
  low.set_gains(g);
  CHECK(low.flat());
}

struct Src {
  std::vector<int16_t> data;  // interleaved
  uint8_t ch;
  size_t pos = 0;
  static size_t pull(int16_t *out, size_t frames, void *ctx) {
    Src &s = *static_cast<Src *>(ctx);
    const size_t have = s.data.size() / s.ch - s.pos;
    if (frames > have) frames = have;
    memcpy(out, s.data.data() + s.pos * s.ch, frames * s.ch * sizeof(int16_t));
    s.pos += frames;
    return frames;
  }
};

static Src sine_src(double rate, double hz, double secs, uint8_t ch, double amp = 0.5) {
  Src s;
  s.ch = ch;
  const size_t n = (size_t)(rate * secs);
  for (size_t i = 0; i < n; i++) {
    const int16_t v = (int16_t)lround(amp * 32767 * sin(2 * kPi * hz * i / rate));
    for (int c = 0; c < ch; c++) s.data.push_back(c ? (int16_t)-v : v);  // right channel inverted
  }
  return s;
}

static void test_resampler() {
  dsp::Resampler rs;
  CHECK(rs.configure(44100, 48000, 2));
  // A 1 kHz and a 15 kHz tone, 44.1 -> 48 kHz: the right amplitude, the right channel inverted, and nothing else
  // (the images of 15 kHz would land at 18.9 kHz).
  for (double hz : {1000.0, 15000.0, 19000.0}) {
    Src s = sine_src(44100, hz, 0.5, 2);
    rs.configure(44100, 48000, 2);
    std::vector<float> l(22000), r(22000);
    const size_t made = rs.process(l.data(), r.data(), l.size(), Src::pull, &s);
    CHECK(made == l.size());
    double amp, resid;
    fit_sine(l, 48000, hz, 200, &amp, &resid);
    NEAR(amp, 0.5, hz > 18000 ? 0.06 : 0.005);  // 19 kHz: the edge of the band, -1 dB
    printf("resampler 44.1 -> 48 kHz, %5.0f Hz: amplitude %.4f, residual %.1f dB\n", hz, amp, resid);
    CHECK(resid < (hz > 18000 ? -40 : -55));
    double ampr, residr;
    fit_sine(r, 48000, hz, 200, &ampr, &residr);
    NEAR(ampr, amp, 0.001);
    double sum = 0;
    for (size_t i = 200; i < l.size(); i++) sum += l[i] + r[i];
    NEAR(sum / l.size(), 0, 1e-4);  // r = -l
  }
  // The input runs dry: what is made is counted, the rest is silence.
  {
    Src s = sine_src(44100, 1000, 0.01, 2);  // 441 frames
    rs.configure(44100, 48000, 2);
    std::vector<float> l(1000, 1.0f), r(1000, 1.0f);
    const size_t made = rs.process(l.data(), r.data(), l.size(), Src::pull, &s);
    CHECK(made > 440 && made < 490);
    CHECK(l[999] == 0 && r[999] == 0);
  }
  // Mono 16 kHz (the call's voice) -> 48 kHz, written to one buffer.
  {
    Src s = sine_src(16000, 1000, 0.5, 1);
    CHECK(rs.configure(16000, 48000, 1));
    std::vector<float> m(20000);
    CHECK(rs.process(m.data(), m.data(), m.size(), Src::pull, &s) == m.size());
    double amp, resid;
    fit_sine(m, 48000, 1000, 200, &amp, &resid);
    NEAR(amp, 0.5, 0.005);
    CHECK(resid < -55);
  }
  // 48 -> 48 kHz (a phone streaming at 48 kHz) passes through, delayed half a window.
  {
    Src s = sine_src(48000, 3000, 0.3, 2);
    CHECK(rs.configure(48000, 48000, 2));
    std::vector<float> l(12000), r(12000);
    rs.process(l.data(), r.data(), l.size(), Src::pull, &s);
    double amp, resid;
    fit_sine(l, 48000, 3000, 200, &amp, &resid);
    NEAR(amp, 0.5, 0.005);
    CHECK(resid < -60);
  }
  // The trim: running 0.2 % fast takes 0.2 % more input for the same output.
  {
    Src a = sine_src(44100, 1000, 12, 2), b = sine_src(44100, 1000, 12, 2);
    std::vector<float> l(480000), r(480000);  // long, so the pull granularity (128 frames) does not count
    rs.configure(44100, 48000, 2);
    rs.process(l.data(), r.data(), l.size(), Src::pull, &a);
    rs.configure(44100, 48000, 2);
    rs.set_trim(1.002);
    rs.process(l.data(), r.data(), l.size(), Src::pull, &b);
    NEAR((double)b.pos / (double)a.pos, 1.002, 0.0005);
  }
}

static void test_decimator() {
  for (int m : {3, 6}) {
    dsp::Decimator d;
    d.configure(48000, m);
    const double out_rate = 48000.0 / m;
    for (double hz : {500.0, 1000.0, out_rate * 0.6, out_rate * 0.9}) {
      std::vector<float> in(48000), out(48000 / m + 1);
      for (size_t i = 0; i < in.size(); i++) in[i] = (float)(0.5 * sin(2 * kPi * hz * i / 48000));
      d.reset();
      const size_t n = d.process(in.data(), in.size(), out.data());
      CHECK(n == 48000u / m);
      out.resize(n);
      const double level = 20 * log10(rms(out, n / 4) / (0.5 / sqrt(2)));
      printf("decimator 48 kHz / %d, %6.0f Hz: %.1f dB\n", m, hz, level);
      if (hz < out_rate * 0.3) {
        NEAR(level, 0, 0.1);  // passes
      } else {
        CHECK(level < -50);  // above the new Nyquist: gone, not folded down
      }
    }
  }
}

static void test_ring() {
  dsp::FrameRing ring;
  CHECK(ring.init(8, 2));
  CHECK(ring.capacity() == 8 && ring.level() == 0);
  int16_t in[20], out[20];
  for (int i = 0; i < 20; i++) in[i] = (int16_t)i;
  CHECK(ring.write(in, 5) == 5);
  CHECK(ring.level() == 5 && ring.space() == 3);
  CHECK(ring.write(in, 5) == 3);  // full
  CHECK(ring.read(out, 2) == 2);
  CHECK(out[0] == 0 && out[3] == 3);
  CHECK(ring.write(in + 10, 2) == 2);  // wraps
  CHECK(ring.read(out, 8) == 8);
  CHECK(out[0] == 4 && out[12] == 10 && out[15] == 13);  // frames 2-4, then 0-2, then the wrapped two
  CHECK(ring.level() == 0);
  ring.write(in, 4);
  ring.clear();
  CHECK(ring.level() == 0);
}

static void test_fader() {
  float g[4];
  dsp::fader_balance(0, 0, g);
  for (float x : g) NEAR(x, 1, 1e-6);
  dsp::fader_balance(10, 0, g);  // all front
  NEAR(g[0], 1, 1e-6);
  NEAR(g[2], 0, 1e-6);
  dsp::fader_balance(-3, 0, g);  // toward the rear: the front 6 dB down
  NEAR(20 * log10(g[0]), -6, 0.01);
  NEAR(g[3], 1, 1e-6);
  dsp::fader_balance(0, -10, g);  // all left
  NEAR(g[0], 1, 1e-6);
  NEAR(g[1], 0, 1e-6);
  NEAR(g[3], 0, 1e-6);
  NEAR(dsp::volume_gain(15), 1, 1e-6);
  NEAR(dsp::volume_gain(0), 0, 1e-6);
  CHECK(dsp::volume_gain(7) < dsp::volume_gain(8));
}

int main() {
  test_eq();
  test_resampler();
  test_decimator();
  test_ring();
  test_fader();
  if (s_fail) {
    fprintf(stderr, "%d check(s) failed\n", s_fail);
    return 1;
  }
  printf("audio_dsp: all tests passed\n");
  return 0;
}
