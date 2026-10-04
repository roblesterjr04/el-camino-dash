// AI voice card front end (ESP32-S3). Docs: docs/hardware/ai_card.md, docs/protocols/ai_link.md.
//
// The logic is in the ai_*.cpp files (host-tested: test/host/run_tests.sh). This file is the hardware glue:
// CAN (elc_can), the I2S rail as a slave, the UART link, the power pins, NVS. IT HAS NOT BEEN COMPILED OR RUN YET:
// the PlatformIO registry was not reachable when it was written. Expect the first build to need small fixes
// (the I2S driver calls are ESP-IDF 5.x's i2s_std.h as bundled with Arduino-ESP32 3.x).

#include <Arduino.h>
#include <Preferences.h>
#include <driver/i2s_std.h>
#include <elc_can.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <string.h>

#include <memory>
#include <vector>

#include "ai_audio.h"
#include "ai_commands.h"
#include "ai_link.h"
#include "ai_pins.h"
#include "ai_policy.h"
#include "ai_power.h"
#include "ai_session.h"
#include "ai_ui.h"
#include "wake_engine.h"

#ifndef AI_BENCH_NODE
#define AI_BENCH_NODE 0
#endif

static const uint32_t kLinkBaud = 1500000;
static const size_t kMicChunk = 320;  // 20 ms at 16 kHz

// ---------------------------------------------------------------------------------------------------- state
static ElcCan s_can;
static ElcChannels s_chan;
static Preferences s_prefs;
static uint8_t s_slot = 0, s_node = 0;

static HardwareSerial &s_link = Serial1;
static ailink::Tx s_tx;
static ailink::Rx s_rx;

static AiPolicy s_policy;
static AiPower s_power;
static AiPowerIn s_pin;

static AiUi *s_ui;

// module state
static bool s_module_ready = false, s_online_up = false, s_throttled = false;
static uint32_t s_ping_ms = 0, s_pong_ms = 0;
static uint8_t s_ping_n = 0;
static int8_t s_soc_c = 0;
static uint8_t s_assistant_state = 0, s_assistant_source = 0;
static bool s_shutdown_ack = false;

// CAN-derived state
static bool s_ignition = false;
static uint32_t s_subscribed = elc_ch_bit(ELC_CH_SYSTEM) | elc_ch_bit(ELC_CH_SENSOR) | elc_ch_bit(ELC_CH_ALERT) |
                               elc_ch_bit(ELC_CH_ONLINE) | elc_ch_bit(ELC_CH_AUDIO_CMD);
static uint16_t s_rail_rate = 48000;

// audio
static StreamBufferHandle_t s_mic_sb;     // 16 kHz mono s16 from the audio task to the link
static StreamBufferHandle_t s_speech_sb;  // rail-format stereo s32 from the link to the audio task
static volatile bool s_mic_stream = false, s_ret_on = false;
static volatile uint32_t s_rail_last_ms = 0, s_played_frames = 0;
static volatile int s_level_db10 = -1000;
static volatile bool s_wake_hit = false;
static volatile uint8_t s_wake_kw = 0;
static i2s_chan_handle_t s_i2s_rx = nullptr, s_i2s_tx = nullptr;
static WakeEngine s_wake;
static std::unique_ptr<aiaudio::Resampler> s_speech_rs;
static uint32_t s_speech_in_rate = 0;
static aiaudio::SpeechPacer s_pacer;
static bool s_speech_end = false;
static uint32_t s_pacer_frames_seen = 0;

// ---------------------------------------------------------------------------------------------------- link send
static void link_send(uint8_t type, const uint8_t *p = nullptr, size_t n = 0) {
  uint8_t w[ailink::kMaxWire];
  const size_t k = s_tx.frame(type, p, n, w, sizeof w);
  if (k) s_link.write(w, k);
}

static void send_hello() {
  uint8_t b[8];
  ailink::Put p(b, sizeof b);
  p.u8(1).u16(ELC_CAN_FW_VERSION).u8(s_slot).u8(s_node).u8((s_ui->settings().mic_muted ? 1 : 0) | (s_pin.aux_present ? 2 : 0));
  link_send(ailink::T_HELLO, b, p.n);
}

static void send_settings() {
  const AiSettings &s = s_ui->settings();
  uint8_t b[8];
  ailink::Put p(b, sizeof b);
  p.u8(s.online).u8(s.speak ? 1 : 0).u8(s.voice_level).u8(0);
  link_send(ailink::T_SETTINGS, b, p.n);
}

static void send_ctx(uint8_t key, int32_t v) {
  uint8_t b[8];
  ailink::Put p(b, sizeof b);
  p.u8(key).u32((uint32_t)v);
  link_send(ailink::T_CTX, b, p.n);
}

static void send_all_ctx(uint32_t now) {
  send_ctx(1, (now - s_rail_last_ms) < 200);
  send_ctx(2, s_ret_on);
  send_ctx(3, s_level_db10);
  send_ctx(4, s_pin.temp_x10);
  send_ctx(5, s_pin.aux_present);
  send_ctx(6, s_rail_rate / 10);
  send_ctx(7, s_ui->settings().mic_muted);
}

// ---------------------------------------------------------------------------------------------------- session glue
struct Io : AiSessionIo {
  void sendListen(uint8_t reason, uint8_t kw) override { const uint8_t b[2] = {reason, kw}; link_send(ailink::T_LISTEN, b, 2); }
  void sendCancel(uint8_t reason) override { link_send(ailink::T_CANCEL, &reason, 1); }
  void sendSpoken(uint8_t state) override { link_send(ailink::T_SPOKEN, &state, 1); }
  void micForward(bool on) override {
    if (on) xStreamBufferReset(s_mic_sb);
    s_mic_stream = on;
    digitalWrite(AI_PIN_LED_MIC, on ? HIGH : LOW);
  }
  void railRequest() override { rail_send(0); }
  void railRelease() override { rail_send(2); }
  void retEnable(bool on) override {
    s_ret_on = on;
    digitalWrite(AI_PIN_RET_OE_N, on ? LOW : HIGH);  // the buffer drives RET only while granted
    if (!on) xStreamBufferReset(s_speech_sb);
  }
  void flushSpeech() override {
    xStreamBufferReset(s_speech_sb);
    s_pacer.reset(s_speech_in_rate ? s_speech_in_rate : 22050);
    s_speech_end = false;
  }
  void notReady() override { /* a chime: AUDIO_CMD PLAY of the "soft chime" sound (sound 2) from the audio sink */
    const uint8_t sink = s_chan.findConsumer(ELC_CH_AUDIO_CMD);
    if (!sink) return;
    ElcChWriter w;
    elc_audio_encode(ElcAudioPlay{sink, 2, 0x01, 100, 1}, w);
    s_chan.send(ELC_CH_AUDIO_CMD, w);
  }
  static void rail_send(uint8_t state) {
    const uint8_t audio = s_chan.findNode(ELC_TYPE_AUDIO_SINK);
    if (!audio) return;
    ElcNetRail r;
    r.dest = audio;
    r.slot = s_slot;
    r.state = state;
    r.rate10 = s_rail_rate / 10;
    uint8_t b[8];
    const size_t n = elc_net_encode_rail(r, b, sizeof b);
    s_chan.send(ELC_CH_NET, b, n);
  }
};
static Io s_io;
static AiSession s_session(s_io);

// ---------------------------------------------------------------------------------------------------- CAN
static void ui_emit(const ElcChWriter &w, void *) { s_chan.send(ELC_CH_UI, w); }

static void on_describe(uint8_t what, void *) {
  if (what & ELC_NET_Q_UI) {
    s_ui->describe();
    s_ui->sendAll();
  }
}

static void on_transfer(const ElcChTransfer &t, void *) {
  const uint32_t now = millis();
  // everything the assistant subscribed to goes to the module as it is
  if (s_module_ready && t.base && (s_subscribed & elc_ch_bit(t.base))) {
    uint8_t b[ELC_CH_MAX_PAYLOAD + 4];
    ailink::Put p(b, sizeof b);
    p.u16(t.base).u8(t.src).bytes(t.data, t.len);
    if (p.ok()) link_send(ailink::T_CAN_RX, b, p.n);
  }
  switch (t.base) {
    case ELC_CH_NET:
      if (t.op() == ELC_NET_RAIL) {
        ElcNetRail r;
        if (elc_net_decode_rail(t.data, t.len, r) && r.dest == s_node) {
          if (r.rate10) s_rail_rate = (uint16_t)(r.rate10 * 10);
          s_session.onRail(r.state, now);
        }
      }
      break;
    case ELC_CH_SYSTEM: {
      ElcSysVehicle v;
      ElcSysPower pw;
      if (elc_sys_decode(t, v)) s_ignition = (v.flags & ELC_VEH_IGNITION) != 0;
      else if (elc_sys_decode(t, pw) && pw.event == ELC_POWER_IGN_OFF) s_ignition = false;  // do not wait for the next VEHICLE
      break;
    }
    case ELC_CH_INPUT: {
      ElcInputKey k;
      if (elc_input_decode(t, k) && k.key == ELC_KEY_F1 && k.gesture == ELC_GEST_CLICK) {
        if (s_session.state() == AiSessionState::Idle) s_session.start(AI_LISTEN_KEY, 0, now);
        else s_session.cancel(AI_CANCEL_KEY, now);
      }
      break;
    }
    case ELC_CH_UI: {
      ElcUiSet set;
      if (elc_ui_decode(t, set) && set.dest == s_node) s_ui->onSet(set, t.src);
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------------- link receive
static void handle_frame(const ailink::Frame &f, uint32_t now) {
  ailink::Get g(f.payload, f.len);
  switch (f.type) {
    case ailink::T_READY:
      s_module_ready = true;
      send_hello();
      send_settings();
      s_pin.hello = true;
      break;
    case ailink::T_PONG: {
      g.u8();
      s_soc_c = (int8_t)g.u8();
      g.u8();
      const uint8_t flags = g.u8();
      s_online_up = flags & 1;
      s_throttled = (flags >> 1) & 1;
      s_pong_ms = now;
      break;
    }
    case ailink::T_SHUTDOWN_ACK: s_shutdown_ack = true; break;
    case ailink::T_STATE:
      s_assistant_state = g.u8();
      s_assistant_source = g.u8();
      if (s_assistant_state == 0) s_session.onAssistantIdle(now);
      break;
    case ailink::T_LISTEN_STOP: s_session.onListenStop(now); break;
    case ailink::T_TEXT: {
      const uint8_t kind = g.u8();
      char text[65];
      size_t n = g.left() < 64 ? g.left() : 64;
      memcpy(text, g.rest(), n);
      text[n] = 0;
      if (kind == 1) s_ui->setText(kUiHeard, text);
      else if (kind == 2) s_ui->setText(kUiAnswer, text);
      else s_ui->setText(kUiAnswer, text);
      break;
    }
    case ailink::T_SPEECH: {
      static const uint32_t kRates[] = {16000, 22050, 24000, 44100, 48000};
      const uint8_t code = g.u8();
      if (code >= 5 || !s_ui->settings().speak) break;
      if (!s_speech_rs || s_speech_in_rate != kRates[code]) {
        s_speech_in_rate = kRates[code];
        s_speech_rs.reset(new aiaudio::Resampler(s_speech_in_rate, s_rail_rate));
        s_pacer.setRate(s_speech_in_rate);
      }
      const size_t n = g.left() / 2;
      std::vector<int16_t> out;
      s_speech_rs->process(reinterpret_cast<const int16_t *>(g.rest()), n, out);
      std::vector<int32_t> st(out.size() * 2);
      aiaudio::s16_to_rail_stereo(out.data(), out.size(), st.data(), s_ui->settings().voice_level);
      xStreamBufferSend(s_speech_sb, st.data(), st.size() * 4, 0);
      s_pacer.received(g.left());
      s_session.onSpeech(g.left(), now);
      break;
    }
    case ailink::T_SPEECH_END:
      s_speech_end = true;
      s_session.onSpeechEnd(now);
      break;
    case ailink::T_SPEECH_CANCEL: s_session.onSpeechCancel(now); break;
    case ailink::T_CAN_TX: {
      const uint8_t tag = g.u8();
      const uint16_t base = g.u16();
      uint8_t result = 1;
      const AiVerdict v = s_policy.check(base, g.rest(), g.left(), now);
      if (v == AiVerdict::Allow) result = s_chan.send(base, g.rest(), g.left()) ? 0 : 3;
      else if (v == AiVerdict::RateLimited) result = 2;
      const uint8_t b[2] = {tag, result};
      link_send(ailink::T_CAN_TX_RESULT, b, 2);
      break;
    }
    case ailink::T_SUBSCRIBE: {
      uint32_t m = 0;
      const uint8_t count = g.u8();
      for (uint8_t i = 0; i < count && !g.bad; i++) {
        const uint16_t base = g.u16();
        if (base) m |= elc_ch_bit(base);
      }
      if (!g.bad) s_subscribed = m;
      break;
    }
    case ailink::T_CTX_REQUEST: send_all_ctx(now); break;
    case ailink::T_LOG: {
      ElcLogLine ln;
      ln.level = g.u8();
      size_t n = g.left() < 120 ? g.left() : 120;
      memcpy(ln.text, g.rest(), n);
      ln.text[n] = 0;
      Serial.printf("[module] %s\n", ln.text);
      break;
    }
    default: break;
  }
}

static void link_poll(uint32_t now) {
  ailink::Frame f;
  while (s_link.available()) {
    if (s_rx.feed((uint8_t)s_link.read(), f)) handle_frame(f, now);
  }
}

// ---------------------------------------------------------------------------------------------------- audio task
static bool i2s_setup() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
  cc.dma_desc_num = 6;
  cc.dma_frame_num = 240;
  cc.auto_clear = true;  // silence, not the last buffer, when nothing is queued
  if (i2s_new_channel(&cc, &s_i2s_tx, &s_i2s_rx) != ESP_OK) return false;
  i2s_std_config_t sc = {};
  sc.clk_cfg.sample_rate_hz = s_rail_rate;  // ignored in slave mode: the audio card's BCK sets the rate
  sc.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
  sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = (gpio_num_t)AI_PIN_I2S_BCK;
  sc.gpio_cfg.ws = (gpio_num_t)AI_PIN_I2S_WS;
  sc.gpio_cfg.dout = (gpio_num_t)AI_PIN_I2S_RET;
  sc.gpio_cfg.din = (gpio_num_t)AI_PIN_I2S_MIC;
  if (i2s_channel_init_std_mode(s_i2s_tx, &sc) != ESP_OK) return false;
  if (i2s_channel_init_std_mode(s_i2s_rx, &sc) != ESP_OK) return false;
  return i2s_channel_enable(s_i2s_tx) == ESP_OK && i2s_channel_enable(s_i2s_rx) == ESP_OK;
}

static void audio_task(void *) {
  static int32_t rx[240 * 2];
  static int16_t mono[240];
  static int32_t tx[240 * 2];
  std::unique_ptr<aiaudio::Resampler> mic_rs(new aiaudio::Resampler(s_rail_rate, 16000));
  uint32_t mic_rs_rate = s_rail_rate;
  std::vector<int16_t> r16, pend;
  std::vector<int16_t> wake_buf;
  const size_t wchunk = s_wake.chunk();
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(s_i2s_rx, rx, sizeof rx, &got, 30) != ESP_OK || got == 0) continue;  // no clocks: the rail is idle
    s_rail_last_ms = millis();
    const size_t frames = got / 8;
    aiaudio::rail_left_to_s16(rx, frames, mono);
    if (mic_rs_rate != s_rail_rate) { mic_rs.reset(new aiaudio::Resampler(s_rail_rate, 16000)); mic_rs_rate = s_rail_rate; }
    mic_rs->process(mono, frames, r16);
    s_level_db10 = aiaudio::level_dbfs10(r16.data(), r16.size());
    if (s_mic_stream) {
      xStreamBufferSend(s_mic_sb, r16.data(), r16.size() * 2, 0);
    } else if (wchunk && !s_ret_on && !s_ui->settings().mic_muted && s_ui->settings().wake_word) {
      // the wake word runs only while the card is neither listening nor speaking (the card's own voice must not trigger it)
      wake_buf.insert(wake_buf.end(), r16.begin(), r16.end());
      while (wake_buf.size() >= wchunk) {
        const int kw = s_wake.detect(wake_buf.data());
        wake_buf.erase(wake_buf.begin(), wake_buf.begin() + wchunk);
        if (kw) { s_wake_kw = (uint8_t)kw; s_wake_hit = true; }
      }
    } else {
      wake_buf.clear();
    }
    if (s_ret_on) {
      const size_t want = frames * 8;
      const size_t k = xStreamBufferReceive(s_speech_sb, tx, want, 0);
      if (k) {
        size_t w = 0;
        i2s_channel_write(s_i2s_tx, tx, k & ~7u, &w, 20);
        s_played_frames += w / 8;
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------- setup / loop
static void load_settings() {
  AiSettings &s = s_ui->settings();
  s.enabled = s_prefs.getBool("enabled", true);
  s.wake_word = s_prefs.getBool("wake", true);
  s.online = s_prefs.getUChar("online", 0);
  s.mic_muted = s_prefs.getBool("mute", false);
  s.speak = s_prefs.getBool("speak", true);
  s.voice_level = s_prefs.getUChar("level", 80);
}
static void save_settings() {
  const AiSettings &s = s_ui->settings();
  s_prefs.putBool("enabled", s.enabled);
  s_prefs.putBool("wake", s.wake_word);
  s_prefs.putUChar("online", s.online);
  s_prefs.putBool("mute", s.mic_muted);
  s_prefs.putBool("speak", s.speak);
  s_prefs.putUChar("level", s.voice_level);
}

void setup() {
  Serial.begin(115200);
  pinMode(AI_PIN_BUCK_EN, OUTPUT);
  digitalWrite(AI_PIN_BUCK_EN, LOW);  // the module is off until the sequencer says so
  pinMode(AI_PIN_SHUTDOWN_REQ, OUTPUT);
  digitalWrite(AI_PIN_SHUTDOWN_REQ, LOW);
  pinMode(AI_PIN_RET_OE_N, OUTPUT);
  digitalWrite(AI_PIN_RET_OE_N, HIGH);  // RET buffer off
  pinMode(AI_PIN_ALIVE, INPUT);
  pinMode(AI_PIN_ATTN, INPUT);
  pinMode(AI_PIN_LED_MODULE, OUTPUT);
  pinMode(AI_PIN_LED_MIC, OUTPUT);
  s_link.setRxBufferSize(8192);
  s_link.begin(kLinkBaud, SERIAL_8N1, AI_PIN_LINK_RX, AI_PIN_LINK_TX);
  s_prefs.begin("ai", false);
  static AiUi ui(ui_emit, nullptr);
  s_ui = &ui;
  load_settings();

  s_slot = elc_can_read_card_slot(AI_PIN_SLOT_ID);
  s_node = elc_can_node_for_card_slot(s_slot);
  if (!s_node && AI_BENCH_NODE) s_node = AI_BENCH_NODE;
  Serial.printf("ai card: slot %u, node 0x%02X\n", s_slot, s_node);
  if (!s_node) Serial.println("ai card: no valid slot ID (bench or no main board): staying off the bus (build ai-card-keyonly to bench)");

  s_mic_sb = xStreamBufferCreate(16000, 1);
  s_speech_sb = xStreamBufferCreate(48000 * 8 * 3 / 2, 1);  // 1.5 s of 48 kHz stereo s32
  if (!i2s_setup()) Serial.println("ai card: I2S rail setup failed");
  if (s_wake.begin()) Serial.printf("ai card: wake word '%s', chunk %u\n", s_wake.name(), (unsigned)s_wake.chunk());
  else Serial.println("ai card: no wake-word engine: press-to-talk (INPUT F1) only");
  xTaskCreatePinnedToCore(audio_task, "audio", 8192, nullptr, 5, nullptr, 1);

  if (s_node) {
    ElcNetAnnounce me;
    me.type = ELC_TYPE_VOICE;
    me.product = ELC_PRODUCT_VOICE;
    me.fw = ELC_CAN_FW_VERSION;
    me.uid = (uint32_t)(ESP.getEfuseMac() >> 16) ^ (uint32_t)ESP.getEfuseMac();
    me.produces = elc_ch_bit(ELC_CH_AUDIO_CMD) | elc_ch_bit(ELC_CH_ALERT) | elc_ch_bit(ELC_CH_UI) | elc_ch_bit(ELC_CH_INPUT) |
                  elc_ch_bit(ELC_CH_LOG);
    me.consumes = elc_ch_bit(ELC_CH_SYSTEM) | elc_ch_bit(ELC_CH_SENSOR) | elc_ch_bit(ELC_CH_ONLINE) | elc_ch_bit(ELC_CH_UI) |
                  elc_ch_bit(ELC_CH_INPUT) | elc_ch_bit(ELC_CH_ALERT) | elc_ch_bit(ELC_CH_AUDIO_CMD);
    me.flags = (uint8_t)(s_slot << 1);
    snprintf(me.name, sizeof(me.name), "Assistant");
    s_can.begin(AI_PIN_CAN_TX, AI_PIN_CAN_RX, s_node, (ElcCanNodeType)ELC_TYPE_VOICE);
    s_can.attachChannels(&s_chan, me);
    s_chan.setSlot(s_slot);
    s_chan.onTransfer(on_transfer, nullptr);
    s_chan.onDescribe(on_describe, nullptr);
  }
  s_session.setEnabled(s_ui->settings().enabled, millis());
  s_session.setMicMuted(s_ui->settings().mic_muted, millis());
}

void loop() {
  if (s_node) s_can.poll();
  const uint32_t now = millis();
  link_poll(now);

  // inputs of the sequencer
  s_pin.ignition_on = s_ignition;
  s_pin.enabled = s_ui->settings().enabled;
  s_pin.aux_present = analogReadMilliVolts(AI_PIN_AUX_SENSE) > AI_AUX_PRESENT_MV;
  s_pin.temp_x10 = ai_ntc_temp_x10(analogReadMilliVolts(AI_PIN_NTC), 3300, AI_NTC_R_FIXED, AI_NTC_R0, AI_NTC_BETA);
  s_pin.alive_line = digitalRead(AI_PIN_ALIVE);
  s_pin.shutdown_ack = s_shutdown_ack;
  s_pin.hung = s_module_ready && s_power.running() && (now - s_pong_ms > 3 * 2000UL + 500) && s_pong_ms != 0;
  const AiPowerOut po = s_power.poll(s_pin, now);
  digitalWrite(AI_PIN_BUCK_EN, po.buck_en ? HIGH : LOW);
  digitalWrite(AI_PIN_SHUTDOWN_REQ, po.shutdown_req ? HIGH : LOW);
  if (po.send_shutdown) link_send(ailink::T_SHUTDOWN, &po.reason, 1);
  if (po.fault_raised) Serial.println("ai card: compute module not starting (fault)");
  if (s_power.state() == AiPowerState::Off) {  // the buck is off: forget everything the module said
    s_module_ready = false;
    s_pin.hello = false;
    s_pong_ms = 0;
    s_shutdown_ack = false;
  }
  digitalWrite(AI_PIN_LED_MODULE, s_power.running() ? HIGH : (s_power.state() == AiPowerState::Booting ? (now / 300) & 1 : LOW));

  // keep a running module pinged
  if (s_power.running() && now - s_ping_ms >= 2000) {
    s_ping_ms = now;
    link_send(ailink::T_PING, &s_ping_n, 1);
    s_ping_n++;
    if (s_pong_ms == 0) s_pong_ms = now;
    send_all_ctx(now);
  }

  // session
  s_session.setModuleReady(s_power.running() && s_module_ready, now);
  s_session.setRailUp((now - s_rail_last_ms) < 300, now);
  s_session.setMicMuted(s_ui->settings().mic_muted, now);
  s_session.setEnabled(s_ui->settings().enabled, now);
  if (s_wake_hit) {
    s_wake_hit = false;
    s_session.start(AI_LISTEN_WAKE, s_wake_kw, now);
  }
  // microphone audio to the module, 20 ms at a time
  while (s_mic_stream && xStreamBufferBytesAvailable(s_mic_sb) >= kMicChunk * 2) {
    int16_t pcm[kMicChunk];
    xStreamBufferReceive(s_mic_sb, pcm, sizeof pcm, 0);
    link_send(ailink::T_AUDIO, reinterpret_cast<uint8_t *>(pcm), sizeof pcm);
  }
  // speech credit and drain
  if (s_session.speaking() || s_session.state() == AiSessionState::WaitingRail || s_session.state() == AiSessionState::Thinking) {
    const uint32_t played = s_played_frames;
    s_pacer.played_ms((uint32_t)((uint64_t)(played - s_pacer_frames_seen) * 1000 / s_rail_rate));
    s_pacer_frames_seen = played;
    static uint32_t credit_ms = 0;
    if (now - credit_ms >= 100) {
      credit_ms = now;
      const uint16_t c = s_pacer.credit();
      link_send(ailink::T_SPEECH_CREDIT, reinterpret_cast<const uint8_t *>(&c), 2);  // little-endian on this target
    }
    if (s_session.speaking() && xStreamBufferIsEmpty(s_speech_sb)) s_session.onSpeechDrained(now);
  }
  s_session.poll(now);

  // menu page
  static uint32_t ui_ms = 0;
  if (now - ui_ms >= 1000) {
    ui_ms = now;
    static const char *kStates[] = {"Idle", "Listening", "Thinking", "Speaking"};
    s_ui->setText(kUiStatus, s_ui->settings().enabled ? (s_assistant_state < 4 ? kStates[s_assistant_state] : "?") : "Off");
    s_ui->setText(kUiNet, s_ui->settings().online == 2 ? "Offline only" : (s_online_up ? "Online: ready" : "Offline"));
    char m[40];
    snprintf(m, sizeof m, "%s %d C", s_power.name(), s_power.running() ? (int)s_soc_c : s_pin.temp_x10 / 10);
    s_ui->setText(kUiModule, m);
  }
  if (s_ui->takeDirty()) {
    save_settings();
    if (s_power.running()) send_settings();
  }
  if (s_ui->takeForget() && s_power.running()) {
    const uint8_t b[4] = {s_ui->settings().online, (uint8_t)s_ui->settings().speak, s_ui->settings().voice_level, 1};
    link_send(ailink::T_SETTINGS, b, 4);  // flags bit0: forget the conversation
  }
  delay(1);
}
