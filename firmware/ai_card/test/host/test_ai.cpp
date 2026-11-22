#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "ai_audio.h"
#include "ai_commands.h"
#include "ai_link.h"
#include "ai_policy.h"
#include "ai_power.h"
#include "ai_session.h"
#include "ai_ui.h"

static int g_fail = 0, g_n = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    g_n++;                                                                    \
    if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
  } while (0)

// ------------------------------------------------------------------------------------------------------ link
static void test_link() {
  puts("link");
  // COBS vectors from the reference description
  const uint8_t a[] = {0x00};
  uint8_t e[16], d[16];
  size_t n = ailink::cobs_encode(a, 1, e, sizeof e);
  CHECK(n == 2 && e[0] == 1 && e[1] == 1);
  const uint8_t b[] = {0x11, 0x22, 0x00, 0x33};
  n = ailink::cobs_encode(b, 4, e, sizeof e);
  CHECK(n == 5 && e[0] == 3 && e[1] == 0x11 && e[2] == 0x22 && e[3] == 2 && e[4] == 0x33);
  CHECK(ailink::cobs_decode(e, n, d, sizeof d) == 4 && memcmp(d, b, 4) == 0);
  // 254 non-zero bytes and 255 non-zero bytes round-trip
  for (size_t len : {253u, 254u, 255u, 256u, 600u}) {
    std::vector<uint8_t> in(len), enc(len + len / 254 + 4), dec(len + 4);
    for (size_t i = 0; i < len; i++) in[i] = (uint8_t)(1 + i % 255);
    const size_t en = ailink::cobs_encode(in.data(), len, enc.data(), enc.size());
    CHECK(en > 0);
    for (size_t i = 0; i < en; i++) CHECK(enc[i] != 0);
    CHECK(ailink::cobs_decode(enc.data(), en, dec.data(), dec.size()) == len && memcmp(dec.data(), in.data(), len) == 0);
  }

  // frames, through a byte stream with noise in front
  ailink::Tx tx;
  ailink::Rx rx;
  uint8_t w[ailink::kMaxWire];
  uint8_t payload[700];
  for (size_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i % 7 == 0 ? 0 : i);  // many zeros
  const size_t f1 = tx.frame(ailink::T_AUDIO, payload, 640, w, sizeof w);
  CHECK(f1 > 640 && w[f1 - 1] == 0);
  ailink::Frame fr;
  bool got = false;
  for (uint8_t junk : {0x55, 0x00, 0x00, 0x13}) rx.feed(junk, fr);  // a partial garbage frame, then delimiter
  rx.feed(0, fr);
  for (size_t i = 0; i < f1; i++) got = rx.feed(w[i], fr) || got;
  CHECK(got && fr.type == ailink::T_AUDIO && fr.len == 640 && memcmp(fr.payload, payload, 640) == 0);
  CHECK(rx.frames() == 1);

  // a single flipped bit is rejected, the next frame still parses, and the missing sequence is counted
  uint8_t w2[ailink::kMaxWire], w3[ailink::kMaxWire];
  const uint8_t p2[] = {1, 2, 3};
  const size_t f2 = tx.frame(ailink::T_PING, p2, 3, w2, sizeof w2);
  const size_t f3 = tx.frame(ailink::T_PING, p2, 3, w3, sizeof w3);
  w2[2] ^= 0x10;
  const uint32_t bad_before = rx.bad();
  got = false;
  for (size_t i = 0; i < f2; i++) got = rx.feed(w2[i], fr) || got;
  CHECK(!got && rx.bad() == bad_before + 1);
  for (size_t i = 0; i < f3; i++) got = rx.feed(w3[i], fr) || got;
  CHECK(got && fr.len == 3 && rx.gaps() == 1);

  // too long
  CHECK(tx.frame(ailink::T_AUDIO, payload, ailink::kMaxPayload + 1, w, sizeof w) == 0);
  CHECK(tx.frame(ailink::T_AUDIO, payload, 640, w, 100) == 0);
  // an overlong garbage run is dropped and the parser recovers
  for (int i = 0; i < 5000; i++) rx.feed(0x42, fr);
  rx.feed(0, fr);
  const size_t f4 = tx.frame(ailink::T_PONG, p2, 3, w, sizeof w);
  got = false;
  for (size_t i = 0; i < f4; i++) got = rx.feed(w[i], fr) || got;
  CHECK(got && rx.bad() >= 2);
  // payload helpers
  uint8_t pb[16];
  ailink::Put pp(pb, sizeof pb);
  pp.u8(1).u16(0x0203).u32(0x04050607);
  ailink::Get gg(pb, pp.n);
  CHECK(pp.ok() && gg.u8() == 1 && gg.u16() == 0x0203 && gg.u32() == 0x04050607 && !gg.bad);
  gg.u8();
  CHECK(gg.bad);
}

// ------------------------------------------------------------------------------------------------------ audio
static std::vector<int16_t> tone(double hz, uint32_t rate, size_t n, double amp = 12000) {
  std::vector<int16_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = (int16_t)lrint(amp * sin(2 * M_PI * hz * i / rate));
  return v;
}
static double rms(const std::vector<int16_t> &v, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to; i++) s += (double)v[i] * v[i];
  return sqrt(s / (to - from));
}
static std::vector<int16_t> run(uint32_t in, uint32_t out, const std::vector<int16_t> &x, size_t chunk = 480) {
  aiaudio::Resampler r(in, out);
  std::vector<int16_t> all, o;
  for (size_t i = 0; i < x.size(); i += chunk) {
    r.process(x.data() + i, std::min(chunk, x.size() - i), o);
    all.insert(all.end(), o.begin(), o.end());
  }
  return all;
}
static void test_audio() {
  puts("audio");
  // 48k -> 16k: a 1 kHz tone keeps its level, a 10 kHz tone (would alias to 6 kHz) is gone
  auto y = run(48000, 16000, tone(1000, 48000, 48000));
  CHECK(y.size() > 15900 && y.size() <= 16000);
  const double a = rms(y, 1000, 15000), ref = 12000 / sqrt(2.0);
  CHECK(fabs(a / ref - 1) < 0.03);
  auto z = run(48000, 16000, tone(10000, 48000, 48000));
  CHECK(rms(z, 1000, 15000) < ref * 0.01);  // > 40 dB down
  // 16k -> 48k keeps the level and the length ratio
  auto u = run(16000, 48000, tone(1000, 16000, 16000));
  CHECK(u.size() > 47800 && u.size() <= 48000 && fabs(rms(u, 3000, 45000) / ref - 1) < 0.03);
  // 22.05k -> 44.1k, and the awkward 44.1k -> 16k
  auto v = run(22050, 44100, tone(800, 22050, 22050));
  CHECK(v.size() > 44000 && fabs(rms(v, 3000, 40000) / ref - 1) < 0.03);
  auto q = run(44100, 16000, tone(2000, 44100, 44100));
  CHECK(q.size() > 15900 && fabs(rms(q, 1000, 15000) / ref - 1) < 0.03);
  // chunk size does not change the result
  auto x = tone(1500, 48000, 9600);
  auto c1 = run(48000, 16000, x, 480), c2 = run(48000, 16000, x, 77);
  CHECK(c1 == c2);
  // formats
  int32_t fr[8] = {0x12345600, 111, (int32_t)0xFFFF0000, 222, 0x7FFFFF00, 333, (int32_t)0x80000000, 444};
  int16_t m[4];
  aiaudio::rail_left_to_s16(fr, 4, m);
  CHECK(m[0] == 0x1234 && m[1] == -1 && m[2] == 0x7FFF && m[3] == -32768);
  int32_t st[8];
  aiaudio::s16_to_rail_stereo(m, 4, st, 50);
  CHECK(st[0] == st[1] && st[0] == (int32_t)(0x1234 / 2) << 16 && st[2] == st[3]);
  CHECK(aiaudio::level_dbfs10(tone(1000, 16000, 1600, 32767).data(), 1600) > -80);   // about -3 dBFS
  std::vector<int16_t> silence(100, 0);
  CHECK(aiaudio::level_dbfs10(silence.data(), 100) == -1000);
  // pacing
  aiaudio::SpeechPacer p(1000);
  p.reset(16000);
  CHECK(p.credit() == 32000);
  p.received(16000);  // 0.5 s
  CHECK(p.queued_ms() == 500 && p.credit() == 16000);
  p.played_ms(300);
  CHECK(p.queued_ms() == 200);
  p.received(40000);
  CHECK(p.credit() == 0);
}

// ------------------------------------------------------------------------------------------------------ policy
static void test_policy() {
  puts("policy");
  AiPolicy pol;
  uint32_t t = 1000;
  ElcChWriter w;
  elc_audio_encode(ElcAudioVolume{0x14, 1, 5}, w);
  CHECK(pol.check(ELC_CH_AUDIO_CMD, w.data(), w.size(), t) == AiVerdict::Allow);
  // rate limit: 5 per second, then free again in the next window
  int allowed = 1;
  for (int i = 0; i < 10; i++) allowed += pol.check(ELC_CH_AUDIO_CMD, w.data(), w.size(), t) == AiVerdict::Allow;
  CHECK(allowed == 5 && pol.limited() == 6);
  CHECK(pol.check(ELC_CH_AUDIO_CMD, w.data(), w.size(), t + 1001) == AiVerdict::Allow);
  // never allowed
  uint8_t sys[] = {ELC_SYS_POWER, 1, 5};
  CHECK(pol.check(ELC_CH_SYSTEM, sys, sizeof sys, t) == AiVerdict::Refuse);
  uint8_t sens[] = {ELC_SENSOR_VALUE, 0, 1, 0, 0, 0, 0};
  CHECK(pol.check(ELC_CH_SENSOR, sens, sizeof sens, t) == AiVerdict::Refuse);
  CHECK(pol.check(ELC_CH_BULK, sens, sizeof sens, t) == AiVerdict::Refuse);
  CHECK(pol.check(0x400, sens, sizeof sens, t) == AiVerdict::Refuse);  // a private range
  CHECK(pol.check(ELC_CH_AUDIO_CMD, nullptr, 0, t) == AiVerdict::Refuse);
  // AUDIO_CMD: only VOLUME / MEDIA / SOURCE / low-priority PLAY
  ElcChWriter stop;
  elc_audio_encode(ElcAudioStop{0x14, 1}, stop);
  CHECK(pol.check(ELC_CH_AUDIO_CMD, stop.data(), stop.size(), t + 5000) == AiVerdict::Refuse);
  ElcChWriter play, loud;
  elc_audio_encode(ElcAudioPlay{0x14, 1, 0x01, 100, 1}, play);   // priority 1
  elc_audio_encode(ElcAudioPlay{0x14, 4, 0x04, 100, 0}, loud);   // priority 4: the center's warning chime
  CHECK(pol.check(ELC_CH_AUDIO_CMD, play.data(), play.size(), t + 5000) == AiVerdict::Allow);
  CHECK(pol.check(ELC_CH_AUDIO_CMD, loud.data(), loud.size(), t + 5000) == AiVerdict::Refuse);
  // alerts: advisory at most
  ElcAlertDefine ad;
  ad.alert = 0; ad.severity = ELC_SEV_ADVISORY;
  ElcChWriter a1, a2;
  elc_alert_encode(ad, a1);
  ad.severity = ELC_SEV_WARNING;
  elc_alert_encode(ad, a2);
  CHECK(pol.check(ELC_CH_ALERT, a1.data(), a1.size(), t + 9000) == AiVerdict::Allow);
  CHECK(pol.check(ELC_CH_ALERT, a2.data(), a2.size(), t + 9000) == AiVerdict::Refuse);
  // keys: menu navigation only
  ElcChWriter k1, k2;
  elc_input_encode(ElcInputKey{ELC_KEY_SELECT, ELC_GEST_CLICK, 1}, k1);
  elc_input_encode(ElcInputKey{ELC_KEY_F1, ELC_GEST_CLICK, 2}, k2);
  CHECK(pol.check(ELC_CH_INPUT, k1.data(), k1.size(), t + 9000) == AiVerdict::Allow);
  CHECK(pol.check(ELC_CH_INPUT, k2.data(), k2.size(), t + 9000) == AiVerdict::Refuse);
  // UI: values and text, not new items
  ElcChWriter v1, v2;
  elc_ui_encode(ElcUiValue{1, 5}, v1);
  ElcUiItem item;
  elc_ui_encode(item, v2);
  CHECK(pol.check(ELC_CH_UI, v1.data(), v1.size(), t + 9000) == AiVerdict::Allow);
  CHECK(pol.check(ELC_CH_UI, v2.data(), v2.size(), t + 9000) == AiVerdict::Refuse);
}

// ------------------------------------------------------------------------------------------------------ power
struct PowerBench {
  AiPower p;
  AiPowerIn in;
  AiPowerOut out;
  uint32_t t = 0;
  void step(uint32_t dt) { t += dt; out = p.poll(in, t); }
  void boot_ok() {  // the module answers 5 s after the buck comes on
    in.hello = true;
    in.alive_line = true;
  }
};
static void test_power() {
  puts("power");
  PowerBench b;
  b.in.aux_present = true;
  b.step(100);
  CHECK(b.p.state() == AiPowerState::Off && !b.out.buck_en);  // ignition off
  b.in.ignition_on = true;
  b.step(100);
  CHECK(b.p.state() == AiPowerState::Booting && b.out.buck_en);
  b.step(5000);
  b.boot_ok();
  b.step(100);
  CHECK(b.p.running() && b.out.buck_en && !b.out.shutdown_req);
  // ignition off: clean stop, the buck stays on until ALIVE falls
  b.in.ignition_on = false;
  b.step(100);
  CHECK(b.p.state() == AiPowerState::Stopping && b.out.send_shutdown && b.out.shutdown_req && b.out.reason == 1 && b.out.buck_en);
  b.step(100);
  CHECK(!b.out.send_shutdown && b.out.buck_en);
  b.in.alive_line = false;
  b.step(100);
  CHECK(b.p.state() == AiPowerState::Off && !b.out.buck_en);
  // a module that never stops: cut after the stop timeout
  b = PowerBench();
  b.in.aux_present = true; b.in.ignition_on = true;
  b.step(10); b.step(10); b.boot_ok(); b.step(10);
  CHECK(b.p.running());
  b.in.enabled = false;
  b.step(10);
  CHECK(b.out.reason == 2);
  b.step(AiPower::kStopTimeoutMs - 100);
  CHECK(b.out.buck_en);
  b.step(200);
  CHECK(b.p.state() == AiPowerState::Off && !b.out.buck_en);
  // minimum off time, then it starts again when allowed
  b.in.enabled = true;
  b.step(1000);
  CHECK(b.p.state() == AiPowerState::Off);
  b.step(AiPower::kMinOffMs);
  CHECK(b.p.state() == AiPowerState::Booting);

  // three boots that never answer: fault, alert once, retry after the long wait
  b = PowerBench();
  b.in.aux_present = true; b.in.ignition_on = true;
  int faults = 0;
  for (int i = 0; i < 3; i++) {
    b.step(AiPower::kMinOffMs + 10);
    CHECK(b.p.state() == AiPowerState::Booting);
    b.step(AiPower::kBootTimeoutMs + 10);
    faults += b.out.fault_raised;
  }
  CHECK(b.p.state() == AiPowerState::Fault && faults == 1 && !b.out.buck_en);
  b.step(AiPower::kFaultRetryMs - 1000);
  CHECK(b.p.state() == AiPowerState::Fault);
  b.step(2000);
  b.step(AiPower::kMinOffMs + 10);
  CHECK(b.p.state() == AiPowerState::Booting);

  // hung module: restart (stop, off, boot)
  b = PowerBench();
  b.in.aux_present = true; b.in.ignition_on = true;
  b.step(10); b.step(10); b.boot_ok(); b.step(10);
  b.in.hung = true;
  b.step(10);
  CHECK(b.p.state() == AiPowerState::Stopping && b.out.reason == 4);
  b.in.alive_line = false; b.in.hello = false; b.in.hung = false;
  b.step(10);
  CHECK(b.p.state() == AiPowerState::Off);
  b.step(AiPower::kMinOffMs + 10);
  CHECK(b.p.state() == AiPowerState::Booting && b.out.buck_en);

  // heat: stop at 80 C, allowed again under 65 C (not at 70)
  b = PowerBench();
  b.in.aux_present = true; b.in.ignition_on = true;
  b.step(10); b.step(10); b.boot_ok(); b.step(10);
  b.in.temp_x10 = 810;
  b.step(10);
  CHECK(b.p.state() == AiPowerState::Stopping && b.out.reason == 3);
  b.in.alive_line = false;
  b.step(10);
  b.in.temp_x10 = 700;
  b.step(AiPower::kMinOffMs + 10);
  CHECK(b.p.state() == AiPowerState::Off && b.p.hot());
  b.in.temp_x10 = 640;
  b.step(10);
  CHECK(b.p.state() == AiPowerState::Booting);

  // AUX lost while running: off at once, no wait. Ignition off while still booting: cut at once.
  b = PowerBench();
  b.in.aux_present = true; b.in.ignition_on = true;
  b.step(10); b.step(10); b.boot_ok(); b.step(10);
  b.in.aux_present = false;
  b.step(10);
  CHECK(b.p.state() == AiPowerState::Off && !b.out.buck_en);
  b = PowerBench();
  b.in.aux_present = true; b.in.ignition_on = true;
  b.step(10); b.step(10);
  CHECK(b.p.state() == AiPowerState::Booting);
  b.in.ignition_on = false;
  b.step(10);
  CHECK(b.p.state() == AiPowerState::Off);
}

static void test_ntc() {
  puts("ntc");
  CHECK(ai_ntc_temp_x10(1650, 3300, 10000, 10000, 3950) == 250);   // 10k at 25 C
  const int16_t hot = ai_ntc_temp_x10(570, 3300, 10000, 10000, 3950);  // 2.09k: about 65 C by the beta equation
  CHECK(hot > 640 && hot < 670);
  CHECK(ai_ntc_temp_x10(2800, 3300, 10000, 10000, 3950) < 0);
  CHECK(ai_ntc_temp_x10(0, 3300, 10000, 10000, 3950) < 0 && ai_ntc_temp_x10(3300, 3300, 10000, 10000, 3950) > 1000);
}

// ------------------------------------------------------------------------------------------------------ session
struct FakeIo : AiSessionIo {
  std::string log;
  void add(const char *s) { log += s; log += ' '; }
  void sendListen(uint8_t r, uint8_t) override { add(r == AI_LISTEN_WAKE ? "listen:wake" : "listen:key"); }
  void sendCancel(uint8_t r) override { char b[24]; snprintf(b, sizeof b, "cancel:%d", r); add(b); }
  void sendSpoken(uint8_t s) override { char b[24]; snprintf(b, sizeof b, "spoken:%d", s); add(b); }
  void micForward(bool on) override { add(on ? "mic+" : "mic-"); }
  void railRequest() override { add("req"); }
  void railRelease() override { add("rel"); }
  void retEnable(bool on) override { add(on ? "ret+" : "ret-"); }
  void flushSpeech() override { add("flush"); }
  void notReady() override { add("notready"); }
  bool has(const char *s) const { return log.find(s) != std::string::npos; }
};
static void test_session() {
  puts("session");
  FakeIo io;
  AiSession s(io);
  uint32_t t = 1000;
  CHECK(!s.start(AI_LISTEN_WAKE, 1, t) && io.has("notready"));  // nothing ready yet
  s.setModuleReady(true, t);
  s.setRailUp(true, t);
  io.log.clear();
  // the whole happy path
  CHECK(s.start(AI_LISTEN_WAKE, 1, t) && s.listening() && io.has("listen:wake") && io.has("mic+"));
  CHECK(!s.start(AI_LISTEN_WAKE, 1, t));  // already busy
  s.onListenStop(t += 3000);
  CHECK(s.state() == AiSessionState::Thinking && io.has("mic-"));
  s.onSpeech(640, t += 1500);
  CHECK(s.state() == AiSessionState::WaitingRail && io.has("req"));
  s.onRail(1, t += 50);
  CHECK(s.speaking() && io.has("ret+") && io.has("spoken:1"));
  s.onSpeech(640, t += 100);
  s.onSpeechEnd(t += 100);
  s.onSpeechDrained(t += 100);
  CHECK(s.state() == AiSessionState::Idle && io.has("spoken:0") && io.has("ret-") && io.has("rel"));

  // the audio card denies the rail: no sound, the module is told
  io.log.clear();
  s.start(AI_LISTEN_KEY, 0, t += 100);
  s.onListenStop(t += 1000);
  s.onSpeech(640, t += 100);
  s.onRail(2, t += 10);
  CHECK(s.state() == AiSessionState::Idle && io.has("spoken:2") && io.has("cancel:5") && !io.has("ret+"));
  // a grant that never comes
  io.log.clear();
  s.start(AI_LISTEN_KEY, 0, t += 100);
  s.onListenStop(t += 100);
  s.onSpeech(640, t += 100);
  s.poll(t += AiSession::kRailWaitMs + 1);
  CHECK(s.state() == AiSessionState::Idle && io.has("cancel:5"));
  // the grant is withdrawn mid-speech: RET off first, then the rest
  io.log.clear();
  s.start(AI_LISTEN_KEY, 0, t += 100);
  s.onListenStop(t += 100);
  s.onSpeech(640, t += 100);
  s.onRail(1, t += 10);
  s.onRail(2, t += 10);
  CHECK(s.state() == AiSessionState::Idle && io.has("ret-") && io.has("cancel:4") && io.has("rel") == false);
  // timeouts
  io.log.clear();
  s.start(AI_LISTEN_WAKE, 1, t += 100);
  s.poll(t += AiSession::kListenMaxMs + 1);
  CHECK(s.state() == AiSessionState::Idle && io.has("cancel:2"));
  s.start(AI_LISTEN_WAKE, 1, t += 100);
  s.onListenStop(t += 100);
  s.poll(t += AiSession::kThinkMaxMs + 1);
  CHECK(s.state() == AiSessionState::Idle);
  // speech that stalls
  s.start(AI_LISTEN_WAKE, 1, t += 100);
  s.onListenStop(t += 100);
  s.onSpeech(640, t += 100);
  s.onRail(1, t += 10);
  s.poll(t += AiSession::kCreditSilenceMs + 1);
  CHECK(s.state() == AiSessionState::Idle && io.has("ret-"));
  // muting while listening cancels; muted = no start
  io.log.clear();
  s.start(AI_LISTEN_WAKE, 1, t += 100);
  s.setMicMuted(true, t += 10);
  CHECK(s.state() == AiSessionState::Idle && io.has("cancel:3"));
  CHECK(!s.start(AI_LISTEN_WAKE, 1, t += 10));
  s.setMicMuted(false, t);
  // a key interrupts speech and listens again; the wake word does not
  s.start(AI_LISTEN_WAKE, 1, t += 100);
  s.onListenStop(t += 100);
  s.onSpeech(640, t += 100);
  s.onRail(1, t += 10);
  CHECK(!s.start(AI_LISTEN_WAKE, 1, t += 10) && s.speaking());
  io.log.clear();
  CHECK(s.start(AI_LISTEN_KEY, 0, t += 10) && s.listening() && io.has("flush") && io.has("ret-"));
  // the rail disappears while speaking; the module goes away while listening
  s.cancel(AI_CANCEL_KEY, t += 10);
  s.start(AI_LISTEN_WAKE, 1, t += 100);
  s.setModuleReady(false, t += 10);
  CHECK(s.state() == AiSessionState::Idle);
  s.setModuleReady(true, t += 10);
  // an answer with no sound: back to idle
  s.start(AI_LISTEN_WAKE, 1, t += 100);
  s.onListenStop(t += 100);
  s.onSpeechEnd(t += 100);
  CHECK(s.state() == AiSessionState::Idle);
}

// ------------------------------------------------------------------------------------------------------ ui
struct UiLog {
  std::vector<std::vector<uint8_t>> tx;
};
static void ui_emit(const ElcChWriter &w, void *ctx) {
  static_cast<UiLog *>(ctx)->tx.emplace_back(w.data(), w.data() + w.size());
}
static void test_ui() {
  puts("ui");
  UiLog log;
  AiUi ui(ui_emit, &log);
  ui.describe();
  CHECK(log.tx.size() == kUiCount);
  ElcChTransfer t;
  t.base = ELC_CH_UI;
  t.data = log.tx[kUiOnline].data();
  t.len = log.tx[kUiOnline].size();
  ElcUiItem it;
  CHECK(elc_ui_decode(t, it) && it.kind == ELC_UI_CHOICE && it.option_count() == 3 && (it.flags & ELC_UI_F_SETTING));
  log.tx.clear();
  ui.sendAll();
  CHECK(log.tx.size() == kUiCount - 1);  // everything but the action
  // SET: applied, VALUE then RESULT, marked dirty
  log.tx.clear();
  CHECK(ui.onSet(ElcUiSet{0x14, kUiOnline, 2}, 0x03) == ELC_UI_OK && ui.settings().online == 2 && ui.takeDirty() && !ui.takeDirty());
  CHECK(log.tx.size() == 2 && log.tx[0][0] == ELC_UI_VALUE && log.tx[1][0] == ELC_UI_RESULT && log.tx[1][2] == ELC_UI_OK && log.tx[1][3] == 0x03);
  CHECK(ui.onSet(ElcUiSet{0x14, kUiOnline, 3}, 1) == ELC_UI_RANGE && ui.settings().online == 2);
  CHECK(ui.onSet(ElcUiSet{0x14, kUiLevel, 55}, 1) == ELC_UI_OK && ui.settings().voice_level == 55);
  CHECK(ui.onSet(ElcUiSet{0x14, kUiLevel, 53}, 1) == ELC_UI_RANGE);  // off the step
  CHECK(ui.onSet(ElcUiSet{0x14, kUiStatus, 1}, 1) == ELC_UI_READONLY);
  CHECK(ui.onSet(ElcUiSet{0x14, 99, 1}, 1) == ELC_UI_UNKNOWN);
  CHECK(ui.onSet(ElcUiSet{0x14, kUiMute, 1}, 1) == ELC_UI_OK && ui.settings().mic_muted);
  CHECK(ui.onSet(ElcUiSet{0x14, kUiForget, 1}, 1) == ELC_UI_OK && ui.takeForget() && !ui.takeForget());
  // text: sent on change only, cut to 64
  log.tx.clear();
  ui.setText(kUiAnswer, "It is 72 degrees and sunny.");
  ui.setText(kUiAnswer, "It is 72 degrees and sunny.");
  CHECK(log.tx.size() == 1 && strcmp(ui.text(kUiAnswer), "It is 72 degrees and sunny.") == 0);
  ui.setText(kUiAnswer, std::string(100, 'x').c_str());
  CHECK(strlen(ui.text(kUiAnswer)) == 64);
}

// ------------------------------------------------------------------------------------------------------ commands
static void test_commands() {
  puts("commands");
  ElcChWriter w;
  CHECK(ai_command_build(AI_CMD_VOLUME_DOWN, 0x14, w));
  ElcChTransfer t;
  t.base = ELC_CH_AUDIO_CMD;
  t.data = w.data();
  t.len = w.size();
  ElcAudioVolume v;
  CHECK(elc_audio_decode(t, v) && v.dest == 0x14 && v.action == 1 && v.value == -5);
  ElcChWriter w2;
  CHECK(ai_command_build(AI_CMD_NEXT, 0x14, w2));
  t.data = w2.data();
  t.len = w2.size();
  ElcAudioMedia m;
  CHECK(elc_audio_decode(t, m) && m.action == ELC_MEDIA_NEXT);
  ElcChWriter w3;
  CHECK(!ai_command_build(AI_CMD_NONE, 0x14, w3));
  // every taught phrase maps to a command the card can build, and passes the policy it would pass if the AI sent it
  AiPolicy pol;
  for (size_t i = 0; i < kAiCommandPhraseCount; i++) {
    ElcChWriter x;
    CHECK(ai_command_build(kAiCommandPhrases[i].id, 0x14, x));
    CHECK(pol.check(ELC_CH_AUDIO_CMD, x.data(), x.size(), i * 1000) == AiVerdict::Allow);
  }
}

int main() {
  test_link();
  test_audio();
  test_policy();
  test_power();
  test_ntc();
  test_session();
  test_ui();
  test_commands();
  printf("%d checks, %d failed\n", g_n, g_fail);
  if (g_fail) return 1;
  puts("all AI card tests passed");
  return 0;
}
