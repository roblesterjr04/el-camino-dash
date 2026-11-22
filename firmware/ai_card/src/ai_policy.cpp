#include "ai_policy.h"

#include <elc_chan.h>
#include <elc_chan_msg.h>

bool AiPolicy::take(Bucket &b, uint16_t per_second, uint32_t now_ms) {
  if (now_ms - b.window_ms >= 1000) {
    b.window_ms = now_ms;
    b.count = 0;
  }
  if (b.count >= per_second) return false;
  b.count++;
  return true;
}

AiVerdict AiPolicy::check(uint16_t base, const uint8_t *p, size_t len, uint32_t now_ms) {
  if (!p || len < 1) { _refused++; return AiVerdict::Refuse; }
  const uint8_t op = p[0];
  Bucket *bucket = nullptr;
  uint16_t rate = 0;
  bool ok = false;
  switch (base) {
    case ELC_CH_AUDIO_CMD:
      rate = 5;
      bucket = &_audio;
      switch (op) {
        case ELC_AUDIO_VOLUME:
        case ELC_AUDIO_MEDIA:
        case ELC_AUDIO_SOURCE: ok = len >= 3; break;
        case ELC_AUDIO_PLAY:  // dest, sound u16, zones:4 | priority:4 (priority is the low nibble)
          ok = len >= 7 && (p[4] & 0x0F) <= kMaxSoundPriority;
          break;
        default: break;  // STOP, STREAM_OPEN and the sink's own messages are not for the assistant
      }
      break;
    case ELC_CH_ALERT:
      rate = 2;
      bucket = &_alert;
      if (op == ELC_ALERT_DEFINE) ok = len >= 3 && p[2] <= kMaxSeverity;  // alert, severity
      else if (op == ELC_ALERT_STATE) ok = len >= 5;                       // the card's own alerts, never above advisory
      break;
    case ELC_CH_UI:
      rate = 5;
      bucket = &_ui;
      ok = (op == ELC_UI_VALUE || op == ELC_UI_TEXT) && len >= 2;
      break;
    case ELC_CH_INPUT:
      rate = 5;
      bucket = &_input;
      if (op == ELC_INPUT_KEY && len >= 3) {
        const uint8_t k = p[1];
        ok = k == ELC_KEY_UP || k == ELC_KEY_DOWN || k == ELC_KEY_SELECT || k == ELC_KEY_BACK || k == ELC_KEY_HOME;
      }
      break;
    case ELC_CH_LOG:
      rate = 10;
      bucket = &_log;
      ok = op == ELC_LOG_LINE && len >= 2;
      break;
    default: break;  // SYSTEM, SENSOR, BULK, NET, ONLINE, legacy IDs, private ranges: never
  }
  if (!ok) { _refused++; return AiVerdict::Refuse; }
  if (!take(*bucket, rate, now_ms)) { _limited++; return AiVerdict::RateLimited; }
  return AiVerdict::Allow;
}
