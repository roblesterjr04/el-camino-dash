#include "audio_engine.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

#include "audio_dsp.h"
#include "chimes.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/i2s_periph.h"

namespace {

constexpr uint32_t kRate = 48000;
constexpr size_t kBlock = 240;   // frames per pass: 5 ms
constexpr int kDmaDescs = 6;     // 30 ms of output queued
constexpr float kFull = 2147483648.0f;  // a 32-bit slot's full scale

// Bluetooth: the ring between A2DP and the engine, at the phone's rate. The engine keeps it near kBtTarget by
// trimming the resampler's ratio (the phone's clock against the card's), so A2DP's bursts never run it dry or full.
#if defined(BOARD_HAS_PSRAM)
constexpr size_t kBtRing = 8192;   // 186 ms at 44.1 kHz, in PSRAM
constexpr size_t kBtTarget = 4096;
#else
constexpr size_t kBtRing = 2048;   // the WROOM board: what the heap can spare
constexpr size_t kBtTarget = 1024;
#endif
constexpr uint32_t kBtWriteWaitMs = 80;  // A2DP waits this long for room before its audio is dropped

// The call: the phone's voice at 8 / 16 kHz, 24 ms of prefill; the microphone back at the call's rate.
constexpr size_t kCallRing = 2048;
constexpr size_t kMicRing = 1024;
constexpr size_t kMicMaxLag = 480;  // samples kept waiting for the stack at most (30 ms at 16 kHz)

constexpr float kDrift = 0.004f;      // trim per unit of relative ring error
constexpr float kDriftMax = 0.003f;   // +-0.3 %: far beyond any crystal, short of an audible pitch change
constexpr float kVoiceDuck = 0.25f;   // music under a voice card (-12 dB)

EnginePins s_pins;
bool s_ok = false;
i2s_chan_handle_t s_tx0 = nullptr, s_rx0 = nullptr, s_tx1 = nullptr, s_rx1 = nullptr;

// ---- settings posted to the engine (under s_mux; the task copies them at each block)
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct Settings {
  Music music = Music::Off;
  bool rail_voice = false;
  uint8_t volume = 8;
  bool muted = false;
  dsp::Eq10::Design eq;  // designed by the caller (double maths, slow on the ESP32), loaded by the task
  uint32_t eq_gen = 0;
  int8_t fader = 0, balance = 0;
  bool call = false;
  uint32_t call_rate = 16000;
  uint32_t call_gen = 0;  // bumps for every call start (the rings are cleared)
  int32_t call_gain_q15 = 32767;
  bool mic_muted = false;
};
Settings s_set;

// ---- streams in
/** A ring fed by a Bluetooth task and played through a resampler, with drift trimming and a prefill. */
struct Player {
  dsp::FrameRing ring;
  dsp::Resampler rs;
  uint8_t ch = 2;
  volatile uint32_t rate = 44100;  // the producer's
  uint32_t cfg_rate = 0;
  size_t target = 0;
  bool primed = false;
  float level_avg = 0;
  uint32_t underruns = 0, overruns = 0, starts = 0;

  static size_t pull(int16_t *out, size_t frames, void *ctx) { return static_cast<Player *>(ctx)->ring.read(out, frames); }

  /** n frames into l / r (r == l for mono). Silence until the ring has its prefill, and again after it runs dry. */
  void render(float *l, float *r, size_t n) {
    if (rate != cfg_rate) {
      rs.configure(rate, kRate, ch);
      cfg_rate = rate;
      primed = false;
    }
    const size_t lvl = ring.level();
    if (!primed) {
      if (lvl < target) {
        silence(l, r, n);
        return;
      }
      primed = true;
      starts++;
      rs.reset();
      rs.set_trim(1.0);
      level_avg = (float)lvl;
    }
    level_avg += 0.02f * ((float)lvl - level_avg);
    float err = kDrift * (level_avg - (float)target) / (float)target;
    err = err > kDriftMax ? kDriftMax : (err < -kDriftMax ? -kDriftMax : err);
    rs.set_trim(1.0 + err);  // fuller than the target: take input a little faster
    if (rs.process(l, r, n, pull, this) < n) {
      underruns++;
      primed = false;  // refill before playing again
    }
  }
  void drain() {
    ring.clear();
    primed = false;
  }
  static void silence(float *l, float *r, size_t n) {
    memset(l, 0, sizeof(float) * n);
    if (r != l) memset(r, 0, sizeof(float) * n);
  }
};
Player s_bt, s_call;
volatile bool s_bt_open = false;       // A2DP has begun its stream (begin .. end)
volatile uint32_t s_bt_write_ms = 0;   // its last write
volatile Music s_music_now = Music::Off;  // what the task plays (for A2DP's writer: wait for room, or drop)

// Car -> phone microphone at the call rate (producer: the engine; consumer: the stack).
int16_t s_mic[kMicRing];
size_t s_mic_w = 0, s_mic_r = 0;
portMUX_TYPE s_mic_mux = portMUX_INITIALIZER_UNLOCKED;

// Statistics (a torn read does no harm).
uint32_t s_blocks = 0, s_busy_us = 0, s_busy_max_us = 0, s_ret_short = 0;
uint32_t s_rx_frames = 0, s_rx_bad = 0, s_tx_frames = 0, s_tx_fail = 0;
float s_ret_peak = 0, s_adc_peak = 0;

// Chimes: the synth under its own mutex.
ChimeSynth s_chimes;
SemaphoreHandle_t s_chime_mu = nullptr;
volatile bool s_chimes_busy = false;

size_t mic_level() { return (s_mic_w - s_mic_r) % kMicRing; }

void mic_push(const float *x, size_t n) {
  portENTER_CRITICAL(&s_mic_mux);
  for (size_t i = 0; i < n; i++) {
    float v = x[i] * 32768.0f;
    v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
    s_mic[s_mic_w] = (int16_t)v;
    s_mic_w = (s_mic_w + 1) % kMicRing;
  }
  while (mic_level() > kMicMaxLag) s_mic_r = (s_mic_r + 1) % kMicRing;  // the stack fell behind: keep latency low
  portEXIT_CRITICAL(&s_mic_mux);
}

void mic_clear() {
  portENTER_CRITICAL(&s_mic_mux);
  s_mic_r = s_mic_w = 0;
  portEXIT_CRITICAL(&s_mic_mux);
}

/** Tables first in internal RAM (read every sample) while the heap can spare it, else PSRAM. */
void *alloc_fast(size_t n) {
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > n + 48 * 1024) {
    if (void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return p;
  }
  if (void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return p;
  return heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

/** Rings in PSRAM when there is some. */
void *alloc_big(size_t n) {
  if (void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return p;
  return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

gpio_num_t pin(int p) { return p < 0 ? I2S_GPIO_UNUSED : (gpio_num_t)p; }

/** One linear ramp per block from the last value to the new one (no zipper noise on volume, fade and duck). */
struct Ramp {
  float v = 0;
  void apply(float *x, size_t n, float to) {
    if (v == to) {
      if (to != 1.0f) {
        for (size_t i = 0; i < n; i++) x[i] *= to;
      }
      return;
    }
    const float d = (to - v) / (float)n;
    float g = v;
    for (size_t i = 0; i < n; i++) {
      g += d;
      x[i] *= g;
    }
    v = to;
  }
};

bool open_i2s() {
  // I2S0: master, full duplex (the front DAC out, the ADC in), MCLK out for the ADC and the rail.
  i2s_chan_config_t c0 = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  c0.dma_desc_num = kDmaDescs;
  c0.dma_frame_num = kBlock;
  c0.auto_clear = true;  // an underrun plays silence, not the last block again
  if (i2s_new_channel(&c0, &s_tx0, s_pins.din >= 0 ? &s_rx0 : nullptr) != ESP_OK) return false;
  i2s_std_config_t cfg = {};
  cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);  // MCLK = 256 fS
  if (s_pins.mclk >= 0) cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;  // a clean master clock for the ADC and the rail
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  cfg.gpio_cfg.mclk = pin(s_pins.mclk);
  cfg.gpio_cfg.bclk = pin(s_pins.bck);
  cfg.gpio_cfg.ws = pin(s_pins.ws);
  cfg.gpio_cfg.dout = pin(s_pins.dout);
  cfg.gpio_cfg.din = pin(s_pins.din);
  if (i2s_channel_init_std_mode(s_tx0, &cfg) != ESP_OK) return false;
  if (s_rx0 && i2s_channel_init_std_mode(s_rx0, &cfg) != ESP_OK) return false;

  // I2S1: a slave on the same BCK / WS (the rear DAC out, the rail's RET in). The driver turns the clock pins into
  // inputs for it; they stay I2S0's outputs as well (below), so I2S1 hears exactly the clock I2S0 sends.
  if (s_pins.rear_dout >= 0 || s_pins.ret_din >= 0) {
    i2s_chan_config_t c1 = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    c1.dma_desc_num = kDmaDescs;
    c1.dma_frame_num = kBlock;
    c1.auto_clear = true;
    if (i2s_new_channel(&c1, s_pins.rear_dout >= 0 ? &s_tx1 : nullptr, s_pins.ret_din >= 0 ? &s_rx1 : nullptr) != ESP_OK) {
      return false;
    }
    i2s_std_config_t c = cfg;
    c.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    c.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    c.gpio_cfg.dout = pin(s_pins.rear_dout);
    c.gpio_cfg.din = pin(s_pins.ret_din);
    if (s_tx1 && i2s_channel_init_std_mode(s_tx1, &c) != ESP_OK) return false;
    if (s_rx1 && i2s_channel_init_std_mode(s_rx1, &c) != ESP_OK) return false;
    const int clk[2] = {s_pins.bck, s_pins.ws};
    const uint8_t sig[2] = {i2s_periph_signal[0].m_tx_bck_sig, i2s_periph_signal[0].m_tx_ws_sig};
    for (int i = 0; i < 2; i++) {
      gpio_set_direction((gpio_num_t)clk[i], GPIO_MODE_INPUT_OUTPUT);
      esp_rom_gpio_connect_out_signal(clk[i], sig[i], false, false);
    }
  }

  // Start the front and rear at the same frame: both TX queues full of silence, the slave first (it waits for the
  // clock), then the master, whose first edge starts both. They are written in step from then on.
  static int32_t zeros[kBlock * 2];
  for (i2s_chan_handle_t tx : {s_tx0, s_tx1}) {
    if (!tx) continue;
    size_t loaded = 0;
    do {
      loaded = 0;
      i2s_channel_preload_data(tx, zeros, sizeof(zeros), &loaded);
    } while (loaded == sizeof(zeros));
  }
  for (i2s_chan_handle_t ch : {s_tx1, s_rx1, s_rx0, s_tx0}) {
    if (ch && i2s_channel_enable(ch) != ESP_OK) return false;
  }
  return true;
}

void engine_task(void *) {
  static int32_t io[kBlock * 2], rear[kBlock * 2], adc[kBlock * 2], ret[kBlock * 2];
  static float ml[kBlock], mr[kBlock], vl[kBlock], vr[kBlock], mic[kBlock], mic_out[kBlock];
  static int16_t pcm[kBlock * 2];
  dsp::Eq10 eq;
  eq.set_rate(kRate);
  dsp::Decimator dec;
  Ramp vol, fade, duck, voice_gain;
  Settings cur;
  uint32_t eq_gen = 0, call_gen = 0;
  Music music = Music::Off;
  bool call = false;
  float fb[4] = {1, 1, 1, 1};

  for (;;) {
    // ---- the settings for this block
    portENTER_CRITICAL(&s_mux);
    cur = s_set;
    portEXIT_CRITICAL(&s_mux);
    const uint64_t t0 = esp_timer_get_time();
    if (cur.eq_gen != eq_gen) {
      eq_gen = cur.eq_gen;
      eq.load(cur.eq);
    }
    if (cur.music != music) {
      music = cur.music;
      eq.reset();
      fade.v = 0;  // the new source fades in over this block
    }
    s_music_now = music;
    if (cur.call != call || cur.call_gen != call_gen) {
      call = cur.call;
      call_gen = cur.call_gen;
      s_call.drain();
      mic_clear();
      if (call) {
        s_call.rate = cur.call_rate;
        s_call.target = cur.call_rate * 24 / 1000;
        dec.configure(kRate, (int)(kRate / cur.call_rate));
      }
    }
    dsp::fader_balance(s_tx1 ? cur.fader : 0, cur.balance, fb);

    // ---- inputs: the ADC paces the loop when there is one (else the DAC's write below does)
    size_t got = 0;
    if (s_rx0) {
      if (i2s_channel_read(s_rx0, adc, sizeof(adc), &got, pdMS_TO_TICKS(50)) != ESP_OK || got != sizeof(adc)) {
        memset(adc, 0, sizeof(adc));
      }
      const int mc = s_pins.mic_right ? 1 : 0;
      for (size_t i = 0; i < kBlock; i++) {
        mic[i] = (float)adc[2 * i + mc] / kFull;
        const float a = fabsf(mic[i]);
        if (a > s_adc_peak) s_adc_peak = a;
      }
    }
    bool have_ret = false;
    if (s_rx1) {
      if (i2s_channel_read(s_rx1, ret, sizeof(ret), &got, pdMS_TO_TICKS(10)) == ESP_OK && got == sizeof(ret)) {
        have_ret = true;
      } else {
        s_ret_short++;
      }
    }

    // ---- the music
    if (music != Music::Bluetooth || call) s_bt.drain();  // nobody listens: keep A2DP from blocking
    if (call) {
      Player::silence(ml, mr, kBlock);
    } else {
      switch (music) {
        case Music::Bluetooth: s_bt.render(ml, mr, kBlock); break;
        case Music::Rail:
          if (have_ret && !cur.rail_voice) {
            for (size_t i = 0; i < kBlock; i++) {
              ml[i] = (float)ret[2 * i] / kFull;
              mr[i] = (float)ret[2 * i + 1] / kFull;
              const float a = fabsf(ml[i]);
              if (a > s_ret_peak) s_ret_peak = a;
            }
          } else {
            Player::silence(ml, mr, kBlock);
          }
          break;
        default: Player::silence(ml, mr, kBlock); break;
      }
    }
    if (music != Music::Off && !call) eq.process(ml, mr, kBlock);
    const float level = cur.muted ? 0.0f : dsp::volume_gain(cur.volume);
    const float d = cur.rail_voice && have_ret ? kVoiceDuck : 1.0f;
    {
      // one gain for both channels: compute it on the left, copy the ramp to the right
      static float g[kBlock];
      for (auto &x : g) x = 1.0f;
      fade.apply(g, kBlock, 1.0f);
      vol.apply(g, kBlock, level);
      duck.apply(g, kBlock, d);
      for (size_t i = 0; i < kBlock; i++) {
        ml[i] *= g[i];
        mr[i] *= g[i];
      }
    }

    // ---- the voice: a call, or a voice card on RET
    bool voice = false;
    float vgain = 0;
    if (call) {
      s_call.render(vl, vl, kBlock);
      memcpy(vr, vl, sizeof(vr));
      vgain = (float)cur.call_gain_q15 / 32767.0f;
      voice = true;
    } else if (cur.rail_voice && have_ret) {
      for (size_t i = 0; i < kBlock; i++) {
        vl[i] = (float)ret[2 * i] / kFull;
        vr[i] = (float)ret[2 * i + 1] / kFull;
      }
      vgain = level;
      voice = true;
    }
    if (voice) {
      static float g[kBlock];
      for (auto &x : g) x = 1.0f;
      voice_gain.apply(g, kBlock, vgain);
      for (size_t i = 0; i < kBlock; i++) {
        vl[i] *= g[i];
        vr[i] *= g[i];
      }
    } else {
      voice_gain.v = 0;  // the next voice fades in
    }

    // ---- mix to 16 bits, the chimes over it all, then fader / balance into the slots
    for (size_t i = 0; i < kBlock; i++) {
      float l = ml[i] + (voice ? vl[i] : 0), r = mr[i] + (voice ? vr[i] : 0);
      l = l > 0.99997f ? 0.99997f : (l < -1.0f ? -1.0f : l);
      r = r > 0.99997f ? 0.99997f : (r < -1.0f ? -1.0f : r);
      pcm[2 * i] = (int16_t)(l * 32768.0f);
      pcm[2 * i + 1] = (int16_t)(r * 32768.0f);
    }
    if (s_chimes_busy) {
      xSemaphoreTake(s_chime_mu, portMAX_DELAY);
      s_chimes.mix(pcm, kBlock, 2, kRate, !call);  // an alert ducks the music, never the caller
      s_chimes_busy = s_chimes.busy();
      xSemaphoreGive(s_chime_mu);
    }
    for (size_t i = 0; i < kBlock; i++) {
      const float l = pcm[2 * i] * 65536.0f, r = pcm[2 * i + 1] * 65536.0f;
      io[2 * i] = (int32_t)(l * fb[0]);
      io[2 * i + 1] = (int32_t)(r * fb[1]);
      rear[2 * i] = (int32_t)(l * fb[2]);
      rear[2 * i + 1] = (int32_t)(r * fb[3]);
    }

    // ---- the microphone to the phone
    if (call) {
      if (cur.mic_muted || !s_rx0) memset(mic, 0, sizeof(mic));  // no ADC: the phone hears silence
      const size_t n = dec.process(mic, kBlock, mic_out);
      mic_push(mic_out, n);
    }

    s_busy_us = (uint32_t)(esp_timer_get_time() - t0);
    if (s_busy_us > s_busy_max_us) s_busy_max_us = s_busy_us;
    size_t wrote = 0;
    i2s_channel_write(s_tx0, io, sizeof(io), &wrote, pdMS_TO_TICKS(50));  // paces the loop without an ADC
    if (s_tx1) i2s_channel_write(s_tx1, rear, sizeof(rear), &wrote, pdMS_TO_TICKS(50));
    s_blocks++;
  }
}

template <typename F>
void post(F f) {
  portENTER_CRITICAL(&s_mux);
  f(s_set);
  portEXIT_CRITICAL(&s_mux);
}

}  // namespace

// ---- the engine ----

bool engine_begin(const EnginePins &pins) {
  s_pins = pins;
  s_chime_mu = xSemaphoreCreateMutex();
  if (pins.rail_en >= 0) {  // off until the clocks run
    gpio_set_direction((gpio_num_t)pins.rail_en, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pins.rail_en, 1);
  }
  dsp::alloc = alloc_big;
  bool ok = s_bt.ring.init(kBtRing, 2) && s_call.ring.init(kCallRing, 1);
  dsp::alloc = alloc_fast;
  s_bt.ch = 2;
  s_bt.target = kBtTarget;
  s_call.ch = 1;
  s_call.rate = 16000;
  ok = ok && s_bt.rs.configure(44100, kRate, 2) && s_call.rs.configure(16000, kRate, 1);
  s_bt.cfg_rate = 44100;
  s_call.cfg_rate = 16000;
  if (!ok) {
    Serial.println("audio engine: out of memory");
    return false;
  }
  if (!open_i2s()) {
    Serial.println("audio engine: I2S setup failed");
    return false;
  }
  if (pins.rail_en >= 0) gpio_set_level((gpio_num_t)pins.rail_en, 0);  // the rail's clocks, MIC and MCLK out
  // Above the A2DP decoder so the output keeps flowing; on the core Bluetooth's controller does not use.
  xTaskCreatePinnedToCore(engine_task, "audio", 6144, nullptr, configMAX_PRIORITIES - 5, nullptr, 1);
  s_ok = true;
  Serial.printf("audio engine: 48 kHz, front%s%s%s\n", s_tx1 ? " + rear" : "", s_rx0 ? ", microphone in" : "",
                s_rx1 ? ", rail RET" : "");
  return true;
}

bool engine_has_rear() { return s_ok && s_tx1; }
bool engine_has_rail() { return s_ok && s_rx1; }

void engine_set_music(Music m) { post([&](Settings &s) { s.music = m; }); }
void engine_set_rail_voice(bool on) { post([&](Settings &s) { s.rail_voice = on; }); }
void engine_set_volume(uint8_t v) { post([&](Settings &s) { s.volume = v > 15 ? 15 : v; }); }
void engine_set_mute(bool m) { post([&](Settings &s) { s.muted = m; }); }

void engine_set_eq(const int8_t db[10]) {
  const dsp::Eq10::Design d = dsp::Eq10::design(db, kRate);
  post([&](Settings &s) {
    s.eq = d;
    s.eq_gen++;
  });
}

void engine_set_fader_balance(int8_t fader, int8_t balance) {
  post([&](Settings &s) {
    s.fader = fader;
    s.balance = balance;
  });
}

bool engine_bt_flowing() { return s_bt_open && millis() - s_bt_write_ms < 300; }

void engine_status(char *out, size_t n) {
  const float ret_db = s_ret_peak > 0 ? 20 * log10f(s_ret_peak) : -99, adc_db = s_adc_peak > 0 ? 20 * log10f(s_adc_peak) : -99;
  s_ret_peak = s_adc_peak = 0;
  static const char *const kMusic[3] = {"off", "bluetooth", "rail"};
  snprintf(out, n,
           "engine %s, music %s, %lu blocks, %lu/%lu us per 5 ms block; bluetooth %lu Hz, ring %u/%u, trim %+.0f ppm, "
           "%lu starts, %lu underruns; RET %s (peak %.0f dBFS, %lu short reads), mic %s (peak %.0f dBFS)",
           s_ok ? "up" : "DOWN", kMusic[(int)s_music_now % 3], (unsigned long)s_blocks, (unsigned long)s_busy_us,
           (unsigned long)s_busy_max_us, (unsigned long)s_bt.cfg_rate, (unsigned)s_bt.ring.level(), (unsigned)kBtTarget,
           (s_bt.rs.trim() - 1.0) * 1e6, (unsigned long)s_bt.starts, (unsigned long)s_bt.underruns,
           s_rx1 ? "in" : "none", ret_db, (unsigned long)s_ret_short, s_rx0 ? "in" : "none", adc_db);
  s_busy_max_us = 0;
}

// ---- A2DP in ----

bool A2dpInStream::begin() {
  s_bt_open = true;
  return true;
}

void A2dpInStream::end() { s_bt_open = false; }

void A2dpInStream::setAudioInfo(audio_tools::AudioInfo info) {
  _info = info;
  if (info.sample_rate) s_bt.rate = info.sample_rate;
}

int A2dpInStream::availableForWrite() { return (int)(s_bt.ring.space() * 4); }

size_t A2dpInStream::write(const uint8_t *data, size_t len) {
  s_bt_write_ms = millis();
  if (!s_ok || _info.channels != 2 || _info.bits_per_sample != 16) return len;
  const int16_t *f = reinterpret_cast<const int16_t *>(data);
  size_t left = len / 4;
  const uint32_t t0 = millis();
  while (left) {
    const size_t w = s_bt.ring.write(f, left);
    f += w * 2;
    left -= w;
    if (!left) break;
    // Full: wait for the engine while it plays Bluetooth (that paces A2DP's decoder); otherwise it drains the ring.
    if (millis() - t0 > kBtWriteWaitMs) {
      s_bt.overruns++;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return len;
}

// ---- call audio ----

void call_audio_start(uint32_t rate) {
  post([&](Settings &s) {
    s.call = true;
    s.call_rate = rate == 8000 ? 8000 : 16000;
    s.call_gen++;
  });
  s_rx_frames = s_rx_bad = s_tx_frames = s_tx_fail = 0;
  s_call.underruns = s_call.overruns = 0;
  Serial.printf("call audio: %lu Hz, mic %s\n", (unsigned long)rate, s_pins.din >= 0 ? "on" : "none (silence)");
}

void call_audio_stop() {
  bool was = false;
  post([&](Settings &s) {
    was = s.call;
    s.call = false;
  });
  if (was) {
    Serial.printf("call audio: stopped (to phone %lu frames, %lu short; from phone %lu, %lu bad; %lu underruns)\n",
                  (unsigned long)s_tx_frames, (unsigned long)s_tx_fail, (unsigned long)s_rx_frames,
                  (unsigned long)s_rx_bad, (unsigned long)s_call.underruns);
  }
}

void call_audio_rx(const uint8_t *pcm, size_t len, bool bad_frame) {
  if (!call_audio_active()) return;
  if (s_rx_frames == 0) Serial.printf("call audio: first rx %u bytes%s\n", (unsigned)len, bad_frame ? " (bad)" : "");
  s_rx_frames++;
  static int16_t buf[256];
  size_t n = len / 2;
  if (n > 256) n = 256;
  if (bad_frame) memset(buf, 0, n * 2);
  else memcpy(buf, pcm, n * 2);
  if (s_call.ring.write(buf, n) < n) s_call.overruns++;  // full (clock drift): the newest is dropped
  if (bad_frame) s_rx_bad++;
}

uint32_t call_audio_tx(uint8_t *buf, uint32_t len) {
  if (!call_audio_active()) return 0;
  const size_t n = len / 2;
  portENTER_CRITICAL(&s_mic_mux);
  const bool have = mic_level() >= n;
  if (have) {
    for (size_t i = 0; i < n; i++) {
      memcpy(buf + 2 * i, &s_mic[s_mic_r], 2);
      s_mic_r = (s_mic_r + 1) % kMicRing;
    }
  }
  portEXIT_CRITICAL(&s_mic_mux);
  if (have) s_tx_frames++;
  else s_tx_fail++;
  return have ? len : 0;  // 0 = nothing this time (the stack asks again on the next frame from the phone)
}

void call_audio_set_mute(bool muted) { post([&](Settings &s) { s.mic_muted = muted; }); }

void call_audio_set_volume(uint8_t vol_0_15) {
  if (vol_0_15 > 15) vol_0_15 = 15;
  // 2.5 dB per step below full: 15 = 0 dB, 1 = -35 dB, 0 = silent.
  const int32_t g = vol_0_15 == 0 ? 0 : (int32_t)(32767.0f * powf(10.0f, -2.5f * (15 - vol_0_15) / 20.0f));
  post([&](Settings &s) { s.call_gain_q15 = g; });
}

bool call_audio_active() {
  portENTER_CRITICAL(&s_mux);
  const bool c = s_set.call;
  portEXIT_CRITICAL(&s_mux);
  return c;
}

void call_audio_status(char *out, size_t n) {
  bool call, muted;
  uint32_t rate;
  portENTER_CRITICAL(&s_mux);
  call = s_set.call;
  muted = s_set.mic_muted;
  rate = s_set.call_rate;
  portEXIT_CRITICAL(&s_mux);
  snprintf(out, n, "%s, %lu Hz, to phone %lu frames (%lu short), from phone %lu (%lu bad), %lu underruns, %lu overruns, "
                   "mic %s%s",
           call ? "on" : "off", (unsigned long)rate, (unsigned long)s_tx_frames, (unsigned long)s_tx_fail,
           (unsigned long)s_rx_frames, (unsigned long)s_rx_bad, (unsigned long)s_call.underruns,
           (unsigned long)s_call.overruns, s_pins.din >= 0 ? "on" : "none", muted ? " (muted)" : "");
}

// ---- chimes ----

bool chime_play(uint8_t node, uint16_t sound, uint8_t priority, uint8_t level_pct, uint8_t repeat) {
  if (!s_chime_mu) return false;
  xSemaphoreTake(s_chime_mu, portMAX_DELAY);
  const bool ok = s_chimes.play(node, sound, priority, level_pct, repeat);
  s_chimes_busy = s_chimes.busy();
  xSemaphoreGive(s_chime_mu);
  return ok;
}

void chime_stop(uint8_t node, uint16_t sound) {
  if (!s_chime_mu) return;
  xSemaphoreTake(s_chime_mu, portMAX_DELAY);
  s_chimes.stop(node, sound);
  xSemaphoreGive(s_chime_mu);
}

void chime_set_volume(uint8_t v_1_15) {
  if (!s_chime_mu) return;
  xSemaphoreTake(s_chime_mu, portMAX_DELAY);
  s_chimes.set_volume(v_1_15);
  xSemaphoreGive(s_chime_mu);
}

bool chime_pop_done(uint8_t &node, uint16_t &sound) {
  if (!s_chime_mu) return false;
  xSemaphoreTake(s_chime_mu, portMAX_DELAY);
  const bool ok = s_chimes.pop_done(node, sound);
  xSemaphoreGive(s_chime_mu);
  return ok;
}

uint8_t chime_count() {
  if (!s_chime_mu) return 0;
  xSemaphoreTake(s_chime_mu, portMAX_DELAY);
  const uint8_t n = s_chimes.count();
  xSemaphoreGive(s_chime_mu);
  return n;
}

bool chime_ducking() {
  if (!s_chime_mu) return false;
  xSemaphoreTake(s_chime_mu, portMAX_DELAY);
  const bool d = s_chimes.ducking();
  xSemaphoreGive(s_chime_mu);
  return d;
}
