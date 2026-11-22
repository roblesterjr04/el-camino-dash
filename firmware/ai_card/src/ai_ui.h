#pragma once

// The card's page in the dash menu (UI channel, docs/protocols/can_channels.md): the settings and what the assistant
// is doing. No Arduino dependencies: host-tested. The glue keeps the settings in NVS (settings() / dirty()).

#include <stddef.h>
#include <stdint.h>

#include <elc_chan.h>
#include <elc_chan_msg.h>

struct AiSettings {
  bool enabled = true;
  bool wake_word = true;
  uint8_t online = 0;          // 0 auto, 1 always online, 2 offline only
  bool mic_muted = false;
  bool speak = true;
  uint8_t voice_level = 80;    // %
};

enum AiUiItem : uint8_t {
  kUiEnabled = 0, kUiWake, kUiOnline, kUiMute, kUiSpeak, kUiLevel,
  kUiStatus, kUiHeard, kUiAnswer, kUiNet, kUiModule, kUiForget,
  kUiCount
};

class AiUi {
 public:
  /** Sends one UI transfer (the glue: ElcChannels::send(ELC_CH_UI, w)). */
  using EmitFn = void (*)(const ElcChWriter &w, void *ctx);

  explicit AiUi(EmitFn emit = nullptr, void *ctx = nullptr) : _emit(emit), _ctx(ctx) {}

  AiSettings &settings() { return _s; }
  const AiSettings &settings() const { return _s; }
  /** True once after a setting changed (save it). */
  bool takeDirty() { const bool d = _dirty; _dirty = false; return d; }
  /** True once after "Forget chat" was pressed. */
  bool takeForget() { const bool d = _forget; _forget = false; return d; }

  /** Every ITEM (boot, and QUERY bit3). */
  void describe();
  /** Every VALUE and TEXT. */
  void sendAll();
  /** SET from the menu: applies, answers with VALUE and RESULT (always). Returns the result code. */
  uint8_t onSet(const ElcUiSet &set, uint8_t asker);
  /** A text readout (cut to 64 bytes); sent when it changed. */
  void setText(uint8_t item, const char *text);
  const char *text(uint8_t item) const { return item < kUiCount ? _text[item] : ""; }
  int32_t value(uint8_t item) const;

 private:
  void emit(const ElcChWriter &w) { if (_emit) _emit(w, _ctx); }
  void sendValue(uint8_t item);
  AiSettings _s;
  EmitFn _emit;
  void *_ctx;
  bool _dirty = false, _forget = false;
  char _text[kUiCount][65] = {{0}};
};
