#include "chimes.h"

#include "audio_dsp.h"

#include <math.h>
#include <string.h>

namespace {

// ---- the sounds ----
// A strike is one bell note: partials at 1x, 2x and 3x the pitch, the upper ones decaying 2x and 3x as fast (so the
// strike starts bright and ends as a pure tone), with a 2 ms attack.
struct StrikeDef {
  uint16_t at_ms;
  float hz;
  float amp;
};
struct SoundDef {
  uint16_t id;
  uint16_t cycle_ms;  // one play, trailing silence included
  float tau_ms;       // the fundamental's decay (to 1/e)
  float w2, w3;       // the 2nd and 3rd partials, relative to the fundamental
  uint8_t n;
  StrikeDef s[ChimeSynth::kMaxStrikes];
};

const SoundDef kSounds[] = {
    // 2 soft chime (advisory): one mellow strike.
    {kChimeSoft, 1200, 300.0f, 0.30f, 0.08f, 1, {{0, 880.0f, 0.8f}}},
    // 3 caution chime: ding-dong, a major third down.
    {kChimeCaution, 1500, 350.0f, 0.25f, 0.06f, 2, {{0, 1318.5f, 0.9f}, {320, 1046.5f, 1.0f}}},
    // 4 warning chime: two bright triplets a second apart (repeat 0 keeps it going until acknowledged).
    {kChimeWarning, 2000, 110.0f, 0.45f, 0.25f, 6,
     {{0, 1760.0f, 1.0f}, {150, 1760.0f, 1.0f}, {300, 1760.0f, 1.0f},
      {1000, 1760.0f, 1.0f}, {1150, 1760.0f, 1.0f}, {1300, 1760.0f, 1.0f}}},
};

const SoundDef *find(uint16_t sound) {
  for (const SoundDef &d : kSounds) {
    if (d.id == sound) return &d;
  }
  return nullptr;
}

constexpr float kHeadroom = 0.4f;      // a full-level strike with its partials peaks near 0.55 of full scale
constexpr float kAttackMs = 2.0f;
constexpr float kEndFadeMs = 30.0f;    // the last play fades out over its last 30 ms (the tail is ~-40 dB by then)
constexpr float kStopFadeMs = 10.0f;   // STOP: no click
constexpr float kMuteRampMs = 10.0f;   // a voice going quiet / audible under a higher priority
constexpr float kDuckLevel = 0.3f;     // music under an alert chime (about -10 dB)
constexpr float kDuckRampMs = 80.0f;
constexpr float kOffLevel = 1e-4f;     // a strike below -80 dB is over

// The chime volume uses the master volume's curve (dsp::volume_gain), so a step sounds the same on both.
using dsp::volume_gain;

constexpr int kLutBits = 10;
float s_lut[(1 << kLutBits) + 1];
bool s_lut_ready = false;

void lut_init() {
  if (s_lut_ready) return;
  for (int i = 0; i <= (1 << kLutBits); i++) s_lut[i] = sinf(6.2831853f * i / (1 << kLutBits));
  s_lut_ready = true;
}

inline float sine(uint32_t phase) {
  const uint32_t i = phase >> (32 - kLutBits);
  const float f = (float)(phase & ((1u << (32 - kLutBits)) - 1)) * (1.0f / (float)(1u << (32 - kLutBits)));
  return s_lut[i] + (s_lut[i + 1] - s_lut[i]) * f;
}

inline uint32_t ms_to_samples(float ms, uint32_t rate) { return (uint32_t)(ms * (float)rate / 1000.0f); }

inline int16_t sat16(float x) {
  if (x > 32767.0f) return 32767;
  if (x < -32768.0f) return -32768;
  return (int16_t)lrintf(x);
}

}  // namespace

bool ChimeSynth::known(uint16_t sound) { return find(sound) != nullptr; }

uint32_t ChimeSynth::length_ms(uint16_t sound) {
  const SoundDef *d = find(sound);
  return d ? d->cycle_ms : 0;
}

bool ChimeSynth::play(uint8_t node, uint16_t sound, uint8_t priority, uint8_t level_pct, uint8_t repeat) {
  const SoundDef *d = find(sound);
  if (!d) return false;
  lut_init();
  Voice *slot = nullptr;
  for (Voice &v : _v) {
    if (v.on && v.node == node && v.sound == sound) {  // the same sound again: it starts over with the new settings
      finish(v);
      slot = &v;
      break;
    }
  }
  if (!slot) {
    for (Voice &v : _v) {
      if (!v.on) {
        slot = &v;
        break;
      }
    }
  }
  if (!slot) {  // all busy: the lowest priority (the oldest of those) makes way, unless it outranks this one
    for (Voice &v : _v) {
      if (!slot || v.prio < slot->prio || (v.prio == slot->prio && v.t > slot->t)) slot = &v;
    }
    if (slot->prio > priority) return false;
    finish(*slot);
  }
  uint8_t top = 0;
  for (const Voice &o : _v) {
    if (o.on && &o != slot && o.prio > top) top = o.prio;
  }
  Voice &v = *slot;
  v = Voice();
  v.on = true;
  v.node = node;
  v.sound = sound;
  v.prio = priority > 4 ? 4 : priority;
  v.level = level_pct > 100 ? 100 : level_pct;
  v.left = repeat;
  v.def = d;
  v.audible = v.prio >= top ? 1.0f : 0.0f;
  return true;
}

void ChimeSynth::stop(uint8_t node, uint16_t sound) {
  for (Voice &v : _v) {
    if (v.on && v.node == node && (sound == kChimeAllOfNode || v.sound == sound) && !v.fade_len) {
      v.fade_len = v.fade_left = v.rate ? ms_to_samples(kStopFadeMs, v.rate) : 1;
      if (!v.fade_len) v.fade_len = v.fade_left = 1;
    }
  }
}

void ChimeSynth::stop_all() {
  for (Voice &v : _v) {
    if (v.on) finish(v);
  }
}

bool ChimeSynth::active() const {
  for (const Voice &v : _v) {
    if (v.on) return true;
  }
  return false;
}

uint8_t ChimeSynth::count() const {
  uint8_t n = 0;
  for (const Voice &v : _v) n += v.on ? 1 : 0;
  return n;
}

bool ChimeSynth::pop_done(uint8_t &node, uint16_t &sound) {
  if (!_done_n) return false;
  node = _done[0].node;
  sound = _done[0].sound;
  memmove(_done, _done + 1, sizeof(Done) * (size_t)(_done_n - 1));
  _done_n--;
  return true;
}

void ChimeSynth::finish(Voice &v) {
  if (!v.on) return;
  v.on = false;
  if (_done_n == sizeof(_done) / sizeof(_done[0])) {  // nobody is reading them: drop the oldest
    memmove(_done, _done + 1, sizeof(Done) * (size_t)(_done_n - 1));
    _done_n--;
  }
  _done[_done_n++] = Done{v.node, v.sound};
}

/** The I2S rate changed under a voice (A2DP's rate, the call's 16 kHz): keep its place in time and its pitch. */
void ChimeSynth::retime(Voice &v, uint32_t rate) {
  const SoundDef &d = *static_cast<const SoundDef *>(v.def);
  if (v.rate) {
    v.t = (uint32_t)((uint64_t)v.t * rate / v.rate);
    v.fade_len = (uint32_t)((uint64_t)v.fade_len * rate / v.rate);
    v.fade_left = (uint32_t)((uint64_t)v.fade_left * rate / v.rate);
    if (v.fade_len && !v.fade_left) v.fade_left = 1;
  }
  for (int i = 0; i < d.n; i++) {
    Strike &s = v.s[i];
    if (v.rate) s.n = (uint32_t)((uint64_t)s.n * rate / v.rate);
    s.k = expf(-1000.0f / (d.tau_ms * (float)rate));
    for (int p = 0; p < 3; p++) s.inc[p] = (uint32_t)((double)d.s[i].hz * (p + 1) * 4294967296.0 / (double)rate);
  }
  v.rate = rate;
}

float ChimeSynth::sample(Voice &v) {
  const SoundDef &d = *static_cast<const SoundDef *>(v.def);
  while (v.next < d.n && v.t >= ms_to_samples(d.s[v.next].at_ms, v.rate)) {
    Strike &s = v.s[v.next++];
    // A strike still ringing from the last play starts again from where it is (phase and level): no click.
    s.a0 = s.on ? s.env * s.att : 0.0f;
    s.on = true;
    s.env = 1.0f;
    s.n = 0;
  }
  const uint32_t attack = ms_to_samples(kAttackMs, v.rate) + 1;
  float out = 0.0f;
  for (int i = 0; i < d.n; i++) {
    Strike &s = v.s[i];
    if (!s.on) continue;
    s.att = s.n >= attack ? 1.0f : s.a0 + (1.0f - s.a0) * (float)s.n / (float)attack;
    const float e1 = s.env, e2 = e1 * e1, e3 = e2 * e1;
    out += d.s[i].amp * s.att * (e1 * sine(s.phase[0]) + d.w2 * e2 * sine(s.phase[1]) + d.w3 * e3 * sine(s.phase[2]));
    for (int p = 0; p < 3; p++) s.phase[p] += s.inc[p];
    s.env *= s.k;
    s.n++;
    if (s.env < kOffLevel) s.on = false;
  }

  float g = 1.0f;
  if (v.fade_len) {
    g = (float)v.fade_left / (float)v.fade_len;
    if (--v.fade_left == 0) {
      finish(v);
      return out * g;
    }
  }
  if (++v.t >= ms_to_samples(d.cycle_ms, v.rate)) {  // one play over
    if (v.left == 1) {
      finish(v);
    } else {
      if (v.left > 1) v.left--;
      v.t = 0;
      v.next = 0;
    }
  } else if (v.left == 1 && !v.fade_len) {
    const uint32_t end = ms_to_samples(d.cycle_ms, v.rate), fade = ms_to_samples(kEndFadeMs, v.rate);
    if (v.t + fade >= end && fade) g = (float)(end - v.t) / (float)fade;
  }
  return out * g;
}

float ChimeSynth::gain(const Voice &v) const {
  return volume_gain(_volume) * (float)v.level / 100.0f * kHeadroom * 32767.0f;
}

void ChimeSynth::mix(int16_t *pcm, size_t frames, uint8_t channels, uint32_t rate, bool duck) {
  if (!rate || !channels) return;
  for (Voice &v : _v) {
    if (v.on && v.rate != rate) retime(v, rate);
  }
  const float mute_step = 1.0f / (float)(ms_to_samples(kMuteRampMs, rate) + 1);
  const float duck_step = (1.0f - kDuckLevel) / (float)(ms_to_samples(kDuckRampMs, rate) + 1);
  float base[kVoices];
  for (int i = 0; i < kVoices; i++) base[i] = _v[i].on ? gain(_v[i]) : 0.0f;
  for (size_t f = 0; f < frames; f++) {
    uint8_t top = 0;
    bool any = false;
    for (const Voice &v : _v) {
      if (v.on && (!any || v.prio > top)) top = v.prio;
      any = any || v.on;
    }
    if (!any && _duck >= 1.0f) {
      // Nothing left to add: the rest of the buffer is the music as it is.
      return;
    }
    float add = 0.0f;
    for (int i = 0; i < kVoices; i++) {
      Voice &v = _v[i];
      if (!v.on) continue;
      const float target = v.prio == top ? 1.0f : 0.0f;  // only the highest priority is heard
      if (v.audible < target) v.audible = fminf(target, v.audible + mute_step);
      if (v.audible > target) v.audible = fmaxf(target, v.audible - mute_step);
      const float g = base[i] * v.audible;
      add += sample(v) * g;  // a lower voice runs on silently, keeping its time
    }
    const float duck_to = any && top >= 2 ? kDuckLevel : 1.0f;
    if (_duck > duck_to) _duck = fmaxf(duck_to, _duck - duck_step);
    if (_duck < duck_to) _duck = fminf(duck_to, _duck + duck_step);
    const float m = duck ? _duck : 1.0f;
    int16_t *fr = pcm + f * channels;
    for (uint8_t c = 0; c < channels; c++) fr[c] = sat16((float)fr[c] * m + add);
  }
}
