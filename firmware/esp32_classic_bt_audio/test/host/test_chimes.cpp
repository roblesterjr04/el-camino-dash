// Host tests for the chimes (arduino/sketches/esp32_audio_bridge/chimes.cpp). Run: ./run_tests.sh [--wav <dir>]

#include <chimes.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static const uint32_t kRate = 44100;

/** Mixes until the synth goes quiet (or max_ms), in blocks as the A2DP path does; stereo. */
static std::vector<int16_t> render(ChimeSynth &c, uint32_t max_ms, uint32_t rate = kRate, size_t block = 512) {
  std::vector<int16_t> out;
  std::vector<int16_t> buf(block * 2);
  const size_t max_frames = (size_t)max_ms * rate / 1000;
  while (out.size() / 2 < max_frames && c.active()) {
    std::fill(buf.begin(), buf.end(), 0);
    c.mix(buf.data(), block, 2, rate, true);
    out.insert(out.end(), buf.begin(), buf.end());
  }
  return out;
}

static int peak(const std::vector<int16_t> &v, size_t from = 0, size_t to = SIZE_MAX) {
  int p = 0;
  for (size_t i = from; i < v.size() && i < to; i++) p = std::max(p, std::abs((int)v[i]));
  return p;
}

/** Largest jump between neighbouring samples of one channel in [from, to) frames. A click is a jump above what the
 *  tone itself makes. */
static int max_step(const std::vector<int16_t> &v, size_t from = 1, size_t to = SIZE_MAX) {
  int m = 0;
  for (size_t i = std::max<size_t>(from, 1); i < v.size() / 2 && i < to; i++) m = std::max(m, std::abs((int)v[2 * i] - (int)v[2 * i - 2]));
  return m;
}

static void test_known_and_lengths() {
  std::printf("known sounds and lengths\n");
  CHECK(ChimeSynth::known(kChimeSoft) && ChimeSynth::known(kChimeCaution) && ChimeSynth::known(kChimeWarning));
  CHECK(!ChimeSynth::known(1) && !ChimeSynth::known(5) && !ChimeSynth::known(0x100));
  CHECK(ChimeSynth::length_ms(kChimeSoft) == 1200);  // tools/cards_sim.py SOUNDS
  CHECK(ChimeSynth::length_ms(kChimeCaution) == 1500);
  CHECK(ChimeSynth::length_ms(kChimeWarning) == 2000);
  ChimeSynth c;
  CHECK(!c.play(0x01, 7, 2, 100, 1));
  CHECK(!c.active());
}

static void test_play_once() {
  std::printf("each chime plays once, for its length, and reports done\n");
  for (uint16_t s : {kChimeSoft, kChimeCaution, kChimeWarning}) {
    ChimeSynth c;
    c.set_volume(15);
    CHECK(c.play(0x01, s, 3, 100, 1));
    CHECK(c.active() && c.count() == 1);
    const auto pcm = render(c, 10000);
    const double ms = 1000.0 * (pcm.size() / 2) / kRate;
    CHECK(std::fabs(ms - ChimeSynth::length_ms(s)) < 15.0);  // within one block
    CHECK(!c.active());
    CHECK(peak(pcm) > 8000);    // loud at full volume
    CHECK(peak(pcm) < 32767);   // and not clipped
    CHECK(max_step(pcm, 1, 20) < 1500);  // the 2 ms attack: no click at the start
    const size_t tail = (size_t)(kRate * 0.005) * 2;  // the last 5 ms: faded out
    CHECK(peak(pcm, pcm.size() - tail) < 300);
    uint8_t n;
    uint16_t snd;
    CHECK(c.pop_done(n, snd) && n == 0x01 && snd == s);
    CHECK(!c.pop_done(n, snd));
  }
}

static void test_repeat_and_stop() {
  std::printf("repeat count, repeat until stop, stop fades\n");
  ChimeSynth c;
  c.set_volume(10);
  c.play(0x14, kChimeCaution, 3, 100, 3);
  auto pcm = render(c, 20000);
  CHECK(std::fabs(1000.0 * (pcm.size() / 2) / kRate - 3 * 1500.0) < 15.0);
  const size_t cycle = kRate * 3 / 2;  // the second play starts while the first still rings: no jump at the seam
  CHECK(max_step(pcm, cycle - 20, cycle + 20) <= max_step(pcm, 1, cycle - 20));

  c.play(0x14, kChimeWarning, 4, 100, 0);
  pcm = render(c, 7000);
  CHECK(c.active());  // still going after 3.5 plays
  c.stop(0x15, kChimeAllOfNode);  // another node's STOP: not ours
  CHECK(c.active());
  c.stop(0x14, kChimeAllOfNode);
  pcm = render(c, 1000);
  CHECK(!c.active());
  CHECK(pcm.size() / 2 <= kRate / 100 + 512);  // gone within the 10 ms fade (and a block)
  uint8_t n;
  uint16_t snd;
  CHECK(c.pop_done(n, snd) && snd == kChimeCaution);
  CHECK(c.pop_done(n, snd) && snd == kChimeWarning);
}

static void test_volume() {
  std::printf("chime volume and level\n");
  auto loudness = [](uint8_t vol, uint8_t prio, uint8_t level) {
    ChimeSynth c;
    c.set_volume(vol);
    c.play(0x01, kChimeSoft, prio, level, 1);
    return peak(render(c, 2000));
  };
  CHECK(loudness(15, 3, 100) > loudness(8, 3, 100));
  CHECK(loudness(8, 3, 100) > loudness(1, 3, 100));
  CHECK(loudness(1, 1, 100) > 50);                    // the lowest setting is quiet, not silent
  CHECK(loudness(0, 1, 100) == loudness(1, 1, 100));  // 0 counts as 1
  CHECK(loudness(8, 1, 100) == loudness(8, 4, 100));  // the priority does not change the level
  CHECK(std::abs(loudness(15, 3, 50) * 2 - loudness(15, 3, 100)) < 4);
}

static void test_priority_and_duck() {
  std::printf("the highest priority is heard; alerts duck the music\n");
  // A parking-style priority-1 sound and a warning: while the warning plays, only it is heard.
  ChimeSynth both, alone;
  both.set_volume(15);
  alone.set_volume(15);
  both.play(0x20, kChimeSoft, 1, 100, 0);
  both.play(0x01, kChimeWarning, 4, 100, 1);
  alone.play(0x01, kChimeWarning, 4, 100, 1);
  std::vector<int16_t> a(4410 * 2, 0), b(4410 * 2, 0);
  both.mix(a.data(), 4410, 2, kRate, false);
  alone.mix(b.data(), 4410, 2, kRate, false);
  int diff = 0;
  for (size_t i = 2 * 441; i < a.size(); i++)  // after the 10 ms the soft chime takes to go quiet diff = std::max(diff, std::abs((int)a[i] - (int)b[i]));
  CHECK(diff < 50);  // the soft chime is muted
  render(both, 3000);  // the warning ends; the soft chime runs on
  CHECK(both.active() && both.count() == 1);

  // Ducking: a steady "music" level under an alert drops to ~30 %, and comes back once the chime is over.
  ChimeSynth d;
  d.set_volume(15);
  d.play(0x01, kChimeSoft, 2, 0, 1);  // level 0: only the duck shows
  std::vector<int16_t> m(kRate * 2 * 2);
  for (auto &x : m) x = 10000;
  d.mix(m.data(), kRate * 2, 2, kRate, true);
  CHECK(m[(kRate / 2) * 2] > 2900 && m[(kRate / 2) * 2] < 3100);  // mid-chime
  CHECK(m.back() == 10000);                                       // after it (and the 80 ms ramp)
  // A priority-1 sound does not duck.
  ChimeSynth e;
  e.play(0x20, kChimeSoft, 1, 0, 1);
  std::vector<int16_t> m2(4410 * 2, 10000);
  e.mix(m2.data(), 4410, 2, kRate, true);
  CHECK(m2.back() == 10000);
}

static void test_rate_change() {
  std::printf("a rate change mid-chime (A2DP to a call's 16 kHz) keeps the timing\n");
  ChimeSynth c;
  c.play(0x01, kChimeCaution, 3, 100, 1);
  std::vector<int16_t> buf(1000 * 2);
  size_t frames44 = 0, frames16 = 0;
  for (int i = 0; i < 20 && c.active(); i++) {  // ~450 ms at 44.1 kHz
    c.mix(buf.data(), 1000, 2, kRate, true);
    frames44 += 1000;
  }
  std::vector<int16_t> mono(160);
  while (c.active()) {
    c.mix(mono.data(), 160, 1, 16000, false);
    frames16 += 160;
  }
  const double ms = 1000.0 * frames44 / kRate + 1000.0 * frames16 / 16000;
  CHECK(std::fabs(ms - 1500.0) < 25.0);
}

static void test_voices() {
  std::printf("voices: replace the same sound, steal the lowest priority when full\n");
  ChimeSynth c;
  c.play(0x01, kChimeSoft, 2, 100, 0);
  c.play(0x01, kChimeSoft, 2, 100, 0);  // the same node and sound: replaced, not doubled
  CHECK(c.count() == 1);
  c.play(0x02, kChimeSoft, 1, 100, 0);
  c.play(0x03, kChimeSoft, 1, 100, 0);
  c.play(0x04, kChimeSoft, 1, 100, 0);
  CHECK(c.count() == 4);
  CHECK(!c.play(0x05, kChimeSoft, 0, 100, 0));  // below everything playing: refused
  CHECK(c.play(0x06, kChimeWarning, 4, 100, 1));  // takes a priority-1 voice
  CHECK(c.count() == 4);
  c.stop_all();
  CHECK(!c.active());
}

static void write_wav(const std::string &path, const std::vector<int16_t> &pcm) {
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::printf("  cannot write %s\n", path.c_str());
    g_failures++;
    return;
  }
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  const uint32_t bytes = (uint32_t)(pcm.size() * 2);
  std::fwrite("RIFF", 1, 4, f);
  u32(36 + bytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16);
  u16(1);
  u16(2);
  u32(kRate);
  u32(kRate * 4);
  u16(4);
  u16(16);
  std::fwrite("data", 1, 4, f);
  u32(bytes);
  std::fwrite(pcm.data(), 2, pcm.size(), f);
  std::fclose(f);
  std::printf("  wrote %s\n", path.c_str());
}

static void wavs(const std::string &dir) {
  std::printf("WAVs (volume 12)\n");
  const struct {
    uint16_t sound;
    uint8_t prio, repeat;
    const char *name;
  } list[] = {{kChimeSoft, 2, 1, "chime2_soft"}, {kChimeCaution, 3, 1, "chime3_caution"},
              {kChimeWarning, 4, 2, "chime4_warning_x2"}};
  for (const auto &w : list) {
    ChimeSynth c;
    c.set_volume(12);
    c.play(0x01, w.sound, w.prio, 100, w.repeat);
    write_wav(dir + "/" + w.name + ".wav", render(c, 10000));
  }
}

int main(int argc, char **argv) {
  test_known_and_lengths();
  test_play_once();
  test_repeat_and_stop();
  test_volume();
  test_priority_and_duck();
  test_rate_change();
  test_voices();
  if (argc == 3 && std::strcmp(argv[1], "--wav") == 0) wavs(argv[2]);
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
