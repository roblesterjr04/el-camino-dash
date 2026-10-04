#pragma once

// One voice session on the ESP32 side: wake word or key -> microphone audio to the module -> the module's answer
// spoken on the rail's RET line (asked for from the audio card, driven only while granted). The logic is separate
// from the hardware (AiSessionIo), so it is host-tested. docs/protocols/ai_link.md, "A session, end to end".

#include <stdint.h>

enum class AiSessionState : uint8_t { Idle, Listening, Thinking, WaitingRail, Speaking };

enum : uint8_t { AI_LISTEN_WAKE = 1, AI_LISTEN_KEY = 2, AI_LISTEN_MENU = 3 };
enum : uint8_t { AI_CANCEL_KEY = 1, AI_CANCEL_TIMEOUT = 2, AI_CANCEL_MUTED = 3, AI_CANCEL_RAIL_LOST = 4, AI_CANCEL_NO_RAIL = 5 };
enum : uint8_t { AI_SPOKEN_DONE = 0, AI_SPOKEN_STARTED = 1, AI_SPOKEN_ABORTED = 2 };

/** What the session does to the world. */
class AiSessionIo {
 public:
  virtual ~AiSessionIo() {}
  virtual void sendListen(uint8_t reason, uint8_t keyword) = 0;
  virtual void sendCancel(uint8_t reason) = 0;
  virtual void sendSpoken(uint8_t state) = 0;
  virtual void micForward(bool on) = 0;          // stream microphone audio to the module
  virtual void railRequest() = 0;                // NET RAIL request to the audio card
  virtual void railRelease() = 0;                // NET RAIL release
  virtual void retEnable(bool on) = 0;           // the RET driver (74LVC125 enable) and the I2S TX
  virtual void flushSpeech() = 0;                // drop queued speech
  virtual void notReady() = 0;                   // heard the wake word but cannot help: a short chime / toast
};

class AiSession {
 public:
  static const uint32_t kListenMaxMs = 12000;
  static const uint32_t kThinkMaxMs = 20000;
  static const uint32_t kRailWaitMs = 2000;
  static const uint32_t kCreditSilenceMs = 2000;

  explicit AiSession(AiSessionIo &io) : _io(io) {}

  // ---- inputs ----
  void setModuleReady(bool ready, uint32_t now);  // module Running and READY
  void setRailUp(bool up, uint32_t now);          // the audio card's BCK / WS are present
  void setMicMuted(bool muted, uint32_t now);
  void setEnabled(bool on, uint32_t now);

  /** A wake word (keyword id) or key press. Returns true when a session started. A key press while speaking
   *  interrupts the speech and starts listening. */
  bool start(uint8_t reason, uint8_t keyword, uint32_t now);
  /** Cancel by the driver (key while listening). */
  void cancel(uint8_t reason, uint32_t now);

  void onListenStop(uint32_t now);               // module: the phrase is complete
  void onSpeech(uint32_t bytes, uint32_t now);   // module: speech arrived (first one asks for the rail)
  void onSpeechEnd(uint32_t now);                // module: no more speech
  void onSpeechCancel(uint32_t now);             // module: stop and drop
  void onRail(uint8_t state, uint32_t now);      // 1 granted, 2 denied / withdrawn
  void onSpeechDrained(uint32_t now);            // the speech queue played out
  void onAssistantIdle(uint32_t now);            // module STATE idle while thinking: it has nothing to say

  void poll(uint32_t now);

  AiSessionState state() const { return _s; }
  const char *name() const;
  bool listening() const { return _s == AiSessionState::Listening; }
  bool speaking() const { return _s == AiSessionState::Speaking; }
  bool wantsMic() const { return _s == AiSessionState::Listening; }

 private:
  void enter(AiSessionState s, uint32_t now) { _s = s; _t0 = now; }
  void idle(uint32_t now);
  void abortSpeech(uint8_t cancel_reason, uint32_t now);

  AiSessionIo &_io;
  AiSessionState _s = AiSessionState::Idle;
  uint32_t _t0 = 0;
  uint32_t _last_speech_ms = 0;
  bool _ready = false, _rail_up = false, _muted = false, _enabled = true;
  bool _speech_end = false;
  bool _granted = false;
};
