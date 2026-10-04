#pragma once

// What the assistant may put on the CAN bus (docs/hardware/ai_card.md, "What the ESP32 lets the AI do"). The compute
// module is untrusted input: this is the only thing between a model's output and the car's bus. Host-tested.

#include <stddef.h>
#include <stdint.h>

enum class AiVerdict : uint8_t { Allow = 0, Refuse = 1, RateLimited = 2 };

class AiPolicy {
 public:
  /** The card's own node (messages "from the AI" are always sent as this node, so a destination of another card's
   *  private range is irrelevant here: only the channels below are allowed at all). */
  AiVerdict check(uint16_t channel_base, const uint8_t *payload, size_t len, uint32_t now_ms);

  uint32_t refused() const { return _refused; }
  uint32_t limited() const { return _limited; }

  /** Highest alert severity the assistant may raise (ELC_SEV_ADVISORY = 1). */
  static const uint8_t kMaxSeverity = 1;
  /** Highest sound priority (the card is an accessory: 0 UI click, 1 accessory). */
  static const uint8_t kMaxSoundPriority = 1;

 private:
  struct Bucket {
    uint32_t window_ms = 0;
    uint16_t count = 0;
  };
  bool take(Bucket &b, uint16_t per_second, uint32_t now_ms);
  Bucket _audio, _alert, _ui, _input, _log;
  uint32_t _refused = 0, _limited = 0;
};
