#include "call_audio.h"

#include "chimes.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include <algorithm>

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr uint32_t kI2sRate = 16000;  // the PCM1808's lowest rate; 8 kHz calls are resampled to it
constexpr size_t kBlock = 120;        // I2S frames per pass: 7.5 ms, one mSBC frame
constexpr size_t kRing = 1024;        // phone -> car samples buffered (64 ms at 16 kHz)
constexpr size_t kPrefill = 3 * kBlock;  // samples (at 16 kHz) to collect before the speaker starts

audio_tools::I2SStream *s_i2s = nullptr;
CallAudioPins s_pins;
SemaphoreHandle_t s_gate_mu = nullptr;
bool s_call_owned = false;  // the call task owns I2S0 (under s_gate_mu)
bool s_a2dp_wants = false;  // A2DP has begun its stream and not ended it (under s_gate_mu)

enum class Req : uint8_t { Start, Stop };
struct Msg {
  Req req;
  uint32_t rate;
};
QueueHandle_t s_q = nullptr;

volatile bool s_running = false;
volatile bool s_muted = false;
volatile int32_t s_gain_q15 = 32767;
volatile uint32_t s_rate = 16000;

// Phone -> car PCM at the call rate (single producer: the stack; single consumer: the call task).
int16_t s_ring[kRing];
size_t s_ring_w = 0, s_ring_r = 0;  // under s_ring_mux
portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

// Counters for `phone` (statistics: a torn read does no harm).
uint32_t s_rx_frames = 0, s_rx_bad = 0, s_tx_frames = 0, s_tx_fail = 0, s_underruns = 0, s_overruns = 0;

// Car -> phone PCM at the call rate (producer: the call task; consumer: the stack, pulling a frame at a time).
constexpr size_t kMicRing = 1024;
constexpr size_t kMicMaxLag = 480;  // samples kept waiting at most (30 ms at 16 kHz): older ones are dropped
int16_t s_mic[kMicRing];
size_t s_mic_w = 0, s_mic_r = 0;  // under s_mic_mux
portMUX_TYPE s_mic_mux = portMUX_INITIALIZER_UNLOCKED;

// Chimes: the synth under its own mutex (taken inside s_gate_mu where both are held). s_chimes_busy lets the audio
// paths skip the mutex when nothing plays; it is only ever set under it.
ChimeSynth s_chimes;
SemaphoreHandle_t s_chime_mu = nullptr;
TaskHandle_t s_chime_task = nullptr;
volatile bool s_chimes_busy = false;
bool s_chime_open = false;              // the chime task opened I2S0 itself (under s_gate_mu)
volatile uint32_t s_a2dp_write_ms = 0;  // A2DP's last write through the gate
constexpr uint32_t kA2dpIdleMs = 100;   // no A2DP write for this long: the chime task plays the chimes itself
constexpr size_t kChimeFrames = 256;    // per pass (A2DP mixing and the chime task)

void chimes_mix(int16_t *pcm, size_t frames, uint8_t channels, uint32_t rate, bool duck) {
  xSemaphoreTake(s_chime_mu, portMAX_DELAY);
  s_chimes.mix(pcm, frames, channels, rate, duck);
  s_chimes_busy = s_chimes.busy();
  xSemaphoreGive(s_chime_mu);
}

size_t ring_level() { return (s_ring_w - s_ring_r) % kRing; }
size_t mic_level() { return (s_mic_w - s_mic_r) % kMicRing; }

void mic_push(const int16_t *x, size_t n) {
  portENTER_CRITICAL(&s_mic_mux);
  for (size_t i = 0; i < n; i++) {
    s_mic[s_mic_w] = x[i];
    s_mic_w = (s_mic_w + 1) % kMicRing;
  }
  while (mic_level() > kMicMaxLag) s_mic_r = (s_mic_r + 1) % kMicRing;  // the stack fell behind: keep latency low
  portEXIT_CRITICAL(&s_mic_mux);
}

size_t ring_pop(int16_t *out, size_t n) {
  portENTER_CRITICAL(&s_ring_mux);
  const size_t have = ring_level();
  if (n > have) n = have;
  for (size_t i = 0; i < n; i++) {
    out[i] = s_ring[s_ring_r];
    s_ring_r = (s_ring_r + 1) % kRing;
  }
  portEXIT_CRITICAL(&s_ring_mux);
  return n;
}

void ring_clear() {
  portENTER_CRITICAL(&s_ring_mux);
  s_ring_r = s_ring_w = 0;
  portEXIT_CRITICAL(&s_ring_mux);
  portENTER_CRITICAL(&s_mic_mux);
  s_mic_r = s_mic_w = 0;
  portEXIT_CRITICAL(&s_mic_mux);
}

gpio_num_t pin(int p) { return p < 0 ? I2S_GPIO_UNUSED : (gpio_num_t)p; }

i2s_chan_handle_t s_tx = nullptr, s_rx = nullptr;

bool open_i2s() {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = 4;
  chan.dma_frame_num = kBlock;
  chan.auto_clear = true;  // an underrun plays silence, not the last block again
  const bool mic = s_pins.din >= 0;
  if (i2s_new_channel(&chan, &s_tx, mic ? &s_rx : nullptr) != ESP_OK) {
    s_tx = s_rx = nullptr;
    return false;
  }
  i2s_std_config_t cfg = {};
  cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kI2sRate);  // MCLK = 256 fS
  if (s_pins.mclk >= 0) {
    cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;  // a clean master clock for the ADC
  }
  // 32-bit slots: BCK = 64 fS, which the PCM1808 (slave) needs; the DACs take 32-bit words as well.
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  cfg.gpio_cfg.mclk = pin(s_pins.mclk);
  cfg.gpio_cfg.bclk = pin(s_pins.bck);
  cfg.gpio_cfg.ws = pin(s_pins.ws);
  cfg.gpio_cfg.dout = pin(s_pins.dout);
  cfg.gpio_cfg.din = pin(s_pins.din);
  bool ok = i2s_channel_init_std_mode(s_tx, &cfg) == ESP_OK;
  if (ok && s_rx) ok = i2s_channel_init_std_mode(s_rx, &cfg) == ESP_OK;
  if (ok) ok = i2s_channel_enable(s_tx) == ESP_OK;
  if (ok && s_rx) ok = i2s_channel_enable(s_rx) == ESP_OK;
  return ok;
}

void close_i2s() {
  if (s_rx) {
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_rx);
    s_rx = nullptr;
  }
  if (s_tx) {
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
  }
}

void take_i2s() {
  xSemaphoreTake(s_gate_mu, portMAX_DELAY);
  s_call_owned = true;
  s_chime_open = false;
  s_i2s->end();
  xSemaphoreGive(s_gate_mu);
}

void give_i2s() {
  xSemaphoreTake(s_gate_mu, portMAX_DELAY);
  s_call_owned = false;
  if (s_a2dp_wants) {
    s_i2s->begin();  // back to A2DP's format (the stream kept its config)
  }
  xSemaphoreGive(s_gate_mu);
}

/** One call, from its audio link coming up until it goes down (or a new start replaces it). */
void run_call(const Msg &m) {
  const bool narrow = m.rate < 16000;  // CVSD: 8 kHz each way
  const size_t call_block = narrow ? kBlock / 2 : kBlock;
  s_rate = m.rate;
  ring_clear();

  take_i2s();
  if (!open_i2s()) {
    Serial.println("call audio: I2S setup failed");
    close_i2s();
    give_i2s();
    return;
  }
  Serial.printf("call audio: %lu Hz, mic %s\n", (unsigned long)m.rate, s_rx ? "on" : "none (silence)");

  static int32_t io[kBlock * 2];       // one I2S block, stereo 32-bit
  static int16_t spk[kBlock];          // phone -> car at the call rate
  static int16_t mic[kBlock];          // car -> phone at the call rate
  bool primed = false;
  int16_t prev = 0;       // last speaker sample (8 kHz upsampling)
  int16_t mic_prev = 0;   // last odd microphone sample (8 kHz decimation)

  while (s_running) {
    Msg next;
    if (xQueuePeek(s_q, &next, 0) == pdTRUE) break;  // stop, or a new start: handled by the caller

    // ---- microphone: one block in (this read paces the loop when there is an ADC)
    size_t got = 0;
    if (s_rx && i2s_channel_read(s_rx, io, sizeof(io), &got, pdMS_TO_TICKS(50)) == ESP_OK && got == sizeof(io)) {
      const int ch = s_pins.mic_right ? 1 : 0;
      if (narrow) {
        // 16 -> 8 kHz: [1 2 1] / 4 around each even sample, then keep it.
        for (size_t j = 0; j < call_block; j++) {
          const int32_t a = io[(4 * j) + ch] >> 16, b = io[(4 * j + 2) + ch] >> 16;
          mic[j] = (int16_t)((mic_prev + 2 * a + b) / 4);
          mic_prev = (int16_t)b;
        }
      } else {
        for (size_t j = 0; j < call_block; j++) mic[j] = (int16_t)(io[2 * j + ch] >> 16);
      }
    } else {
      memset(mic, 0, sizeof(int16_t) * call_block);
    }
    if (s_muted) memset(mic, 0, sizeof(int16_t) * call_block);

    // ---- to the phone: the stack pulls it in its own frame size (call_audio_tx)
    mic_push(mic, call_block);

    // ---- speaker: one block out, after a short prefill so jitter from the radio does not click
    if (!primed && ring_level() >= (narrow ? kPrefill / 2 : kPrefill)) primed = true;
    size_t n = primed ? ring_pop(spk, call_block) : 0;
    if (primed && n < call_block) {
      s_underruns++;
      primed = false;  // refill before playing again
    }
    for (size_t j = n; j < call_block; j++) spk[j] = 0;
    const int32_t g = s_gain_q15;
    for (size_t j = 0; j < call_block; j++) {
      const int32_t s = ((int32_t)spk[j] * g) >> 15;
      if (narrow) {  // 8 -> 16 kHz: linear
        const int32_t mid = ((int32_t)prev + s) / 2;
        io[4 * j] = io[4 * j + 1] = mid << 16;
        io[4 * j + 2] = io[4 * j + 3] = s << 16;
      } else {
        io[2 * j] = io[2 * j + 1] = s << 16;
      }
      prev = (int16_t)s;
    }
    if (s_chimes_busy) {  // the chimes over the phone's voice, at the I2S rate
      static int16_t chime[kBlock];
      memset(chime, 0, sizeof(chime));
      chimes_mix(chime, kBlock, 1, kI2sRate, false);
      for (size_t k = 0; k < kBlock; k++) {
        int32_t v = (io[2 * k] >> 16) + chime[k];
        v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
        io[2 * k] = io[2 * k + 1] = v << 16;
      }
    }
    size_t wrote = 0;
    i2s_channel_write(s_tx, io, sizeof(io), &wrote, pdMS_TO_TICKS(50));  // paces the loop without an ADC
  }

  close_i2s();
  give_i2s();
  ring_clear();
  Serial.printf("call audio: stopped (to phone %lu frames, %lu short; from phone %lu, %lu bad; %lu underruns)\n",
                (unsigned long)s_tx_frames, (unsigned long)s_tx_fail, (unsigned long)s_rx_frames, (unsigned long)s_rx_bad,
                (unsigned long)s_underruns);
}

void call_task(void *) {
  Msg m;
  for (;;) {
    if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) continue;
    while (m.req == Req::Start) {
      s_running = true;
      s_tx_frames = s_tx_fail = s_rx_frames = s_rx_bad = s_underruns = s_overruns = 0;
      run_call(m);
      s_running = false;
      // run_call returns on a queued message (or an I2S failure): take it, a start runs the next call at once.
      if (xQueueReceive(s_q, &m, 0) != pdTRUE) break;
    }
  }
}

/** Plays the chimes when nothing else writes I2S0: no call, and A2DP stopped or silent (a paused phone). Opens I2S0
 *  in A2DP's last format if it is closed, and closes it again when the chimes are over and A2DP does not want it. */
void chime_task(void *) {
  static int16_t buf[kChimeFrames * 2];
  for (;;) {
    if (!s_chimes_busy) {
      xSemaphoreTake(s_gate_mu, portMAX_DELAY);
      if (s_chime_open && !s_a2dp_wants && !s_call_owned) s_i2s->end();
      s_chime_open = false;
      xSemaphoreGive(s_gate_mu);
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // chime_play wakes it
      continue;
    }
    if (millis() - s_a2dp_write_ms < kA2dpIdleMs) {
      vTaskDelay(pdMS_TO_TICKS(10));  // A2DP is writing: its writes carry the chimes
      continue;
    }
    xSemaphoreTake(s_gate_mu, portMAX_DELAY);
    if (s_call_owned) {  // the call task mixes them
      xSemaphoreGive(s_gate_mu);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (!s_i2s->isActive()) {
      s_chime_open = s_i2s->begin();
      if (!s_chime_open) Serial.println("chimes: I2S setup failed");
    }
    const audio_tools::AudioInfo info = s_i2s->audioInfo();
    const uint8_t ch = info.channels == 1 ? 1 : 2;
    if (s_i2s->isActive() && info.bits_per_sample == 16) {
      memset(buf, 0, sizeof(buf));
      chimes_mix(buf, kChimeFrames, ch, info.sample_rate, false);
      s_i2s->write(reinterpret_cast<const uint8_t *>(buf), kChimeFrames * ch * sizeof(int16_t));  // paces the task
      xSemaphoreGive(s_gate_mu);
    } else {
      xSemaphoreGive(s_gate_mu);
      xSemaphoreTake(s_chime_mu, portMAX_DELAY);
      s_chimes.stop_all();  // nowhere to play them
      s_chimes_busy = s_chimes.busy();
      xSemaphoreGive(s_chime_mu);
    }
  }
}

}  // namespace

// ---- gate ----

bool CallGateStream::begin() {
  xSemaphoreTake(s_gate_mu, portMAX_DELAY);
  s_a2dp_wants = true;
  s_chime_open = false;  // A2DP's stream from here on, if the chime task had opened it
  const bool ok = s_call_owned ? true : _i2s.begin();
  xSemaphoreGive(s_gate_mu);
  return ok;
}

void CallGateStream::end() {
  xSemaphoreTake(s_gate_mu, portMAX_DELAY);
  s_a2dp_wants = false;
  if (s_chimes_busy && !s_call_owned) {
    s_chime_open = true;  // a chime is playing: the chime task finishes it and closes I2S0
  } else if (!s_call_owned) {
    _i2s.end();
  }
  xSemaphoreGive(s_gate_mu);
}

void CallGateStream::setAudioInfo(audio_tools::AudioInfo info) {
  xSemaphoreTake(s_gate_mu, portMAX_DELAY);
  _i2s.setAudioInfo(info);  // while a call owns I2S0 the stream is closed, so this only records the format
  xSemaphoreGive(s_gate_mu);
}

size_t CallGateStream::write(const uint8_t *data, size_t len) {
  xSemaphoreTake(s_gate_mu, portMAX_DELAY);
  size_t n = len;
  if (!s_call_owned) {
    s_a2dp_write_ms = millis();
    const audio_tools::AudioInfo info = _i2s.audioInfo();
    if (!s_chimes_busy || info.bits_per_sample != 16 || info.channels != 2) {
      n = _i2s.write(data, len);
    } else {
      // Mix the chimes in (and duck the music under an alert) a block at a time; A2DP's own buffer is const.
      static int16_t buf[kChimeFrames * 2];
      n = 0;
      while (n + 4 <= len) {
        const size_t bytes = std::min(len - n, sizeof(buf)) & ~(size_t)3;
        memcpy(buf, data + n, bytes);
        chimes_mix(buf, bytes / 4, 2, info.sample_rate, true);
        const size_t w = _i2s.write(reinterpret_cast<const uint8_t *>(buf), bytes);
        n += w;
        if (w < bytes) break;
      }
    }
  }
  xSemaphoreGive(s_gate_mu);
  return n;
}

// ---- call audio ----

void call_audio_begin(audio_tools::I2SStream *i2s, const CallAudioPins &pins) {
  s_i2s = i2s;
  s_pins = pins;
  s_gate_mu = xSemaphoreCreateMutex();
  s_q = xQueueCreate(4, sizeof(Msg));
  s_chime_mu = xSemaphoreCreateMutex();
  // Above the A2DP decoder so the call's I2S keeps flowing; on the core Bluetooth's controller does not use.
  xTaskCreatePinnedToCore(call_task, "call_audio", 3072, nullptr, configMAX_PRIORITIES - 5, nullptr, 1);
  xTaskCreatePinnedToCore(chime_task, "chimes", 3072, nullptr, configMAX_PRIORITIES - 6, &s_chime_task, 1);
}

void call_audio_start(uint32_t rate) {
  if (!s_q) return;
  const Msg m{Req::Start, rate};
  xQueueSend(s_q, &m, 0);
}

void call_audio_stop() {
  if (!s_q) return;
  const Msg m{Req::Stop, 0};
  xQueueSend(s_q, &m, 0);
}

void call_audio_rx(const uint8_t *pcm, size_t len, bool bad_frame) {
  if (!s_running) return;
  if (s_rx_frames == 0) Serial.printf("call audio: first rx %u bytes%s\n", (unsigned)len, bad_frame ? " (bad)" : "");
  s_rx_frames++;
  const size_t n = len / 2;
  portENTER_CRITICAL(&s_ring_mux);
  for (size_t i = 0; i < n; i++) {
    if (ring_level() == kRing - 1) {  // full (clock drift): drop the oldest
      s_ring_r = (s_ring_r + 1) % kRing;
      s_overruns++;
    }
    int16_t v = 0;
    if (!bad_frame) memcpy(&v, pcm + 2 * i, 2);
    s_ring[s_ring_w] = v;
    s_ring_w = (s_ring_w + 1) % kRing;
  }
  portEXIT_CRITICAL(&s_ring_mux);
  if (bad_frame) s_rx_bad++;
}

uint32_t call_audio_tx(uint8_t *buf, uint32_t len) {
  if (!s_running) return 0;
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

void call_audio_set_mute(bool muted) { s_muted = muted; }

void call_audio_set_volume(uint8_t vol_0_15) {
  if (vol_0_15 > 15) vol_0_15 = 15;
  // 2.5 dB per step below full: 15 = 0 dB, 1 = -35 dB, 0 = silent.
  s_gain_q15 = vol_0_15 == 0 ? 0 : (int32_t)(32767.0f * powf(10.0f, -2.5f * (15 - vol_0_15) / 20.0f));
}

bool call_audio_active() { return s_running; }

void call_audio_status(char *out, size_t n) {
  snprintf(out, n, "%s, %lu Hz, to phone %lu frames (%lu short), from phone %lu (%lu bad), %lu underruns, %lu overruns, "
                   "mic %s%s",
           s_running ? "on" : "off", (unsigned long)s_rate, (unsigned long)s_tx_frames, (unsigned long)s_tx_fail,
           (unsigned long)s_rx_frames, (unsigned long)s_rx_bad, (unsigned long)s_underruns, (unsigned long)s_overruns,
           s_pins.din >= 0 ? "on" : "none", s_muted ? " (muted)" : "");
}

// ---- chimes ----

bool chime_play(uint8_t node, uint16_t sound, uint8_t priority, uint8_t level_pct, uint8_t repeat) {
  if (!s_chime_mu) return false;
  xSemaphoreTake(s_chime_mu, portMAX_DELAY);
  const bool ok = s_chimes.play(node, sound, priority, level_pct, repeat);
  s_chimes_busy = s_chimes.busy();
  xSemaphoreGive(s_chime_mu);
  if (ok && s_chime_task) xTaskNotifyGive(s_chime_task);
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
