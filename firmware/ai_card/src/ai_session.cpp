#include "ai_session.h"

const char *AiSession::name() const {
  switch (_s) {
    case AiSessionState::Idle: return "idle";
    case AiSessionState::Listening: return "listening";
    case AiSessionState::Thinking: return "thinking";
    case AiSessionState::WaitingRail: return "waiting for the audio card";
    case AiSessionState::Speaking: return "speaking";
  }
  return "?";
}

void AiSession::idle(uint32_t now) {
  if (_granted) {
    _io.retEnable(false);
    _io.railRelease();
    _granted = false;
  }
  _io.micForward(false);
  _speech_end = false;
  enter(AiSessionState::Idle, now);
}

void AiSession::abortSpeech(uint8_t cancel_reason, uint32_t now) {
  _io.flushSpeech();
  _io.sendSpoken(AI_SPOKEN_ABORTED);
  _io.sendCancel(cancel_reason);
  idle(now);
}

void AiSession::setModuleReady(bool ready, uint32_t now) {
  if (_ready && !ready && _s != AiSessionState::Idle) {  // the module went away mid-session
    _io.flushSpeech();
    idle(now);
  }
  _ready = ready;
}

void AiSession::setRailUp(bool up, uint32_t now) {
  if (_rail_up && !up && _s != AiSessionState::Idle) {
    if (_s == AiSessionState::Speaking || _s == AiSessionState::WaitingRail) abortSpeech(AI_CANCEL_RAIL_LOST, now);
    else { _io.sendCancel(AI_CANCEL_RAIL_LOST); idle(now); }
  }
  _rail_up = up;
}

void AiSession::setMicMuted(bool muted, uint32_t now) {
  _muted = muted;
  if (muted && _s == AiSessionState::Listening) {
    _io.sendCancel(AI_CANCEL_MUTED);
    idle(now);
  }
}

void AiSession::setEnabled(bool on, uint32_t now) {
  _enabled = on;
  if (!on && _s != AiSessionState::Idle) {
    if (_s == AiSessionState::Speaking || _s == AiSessionState::WaitingRail) abortSpeech(AI_CANCEL_KEY, now);
    else { _io.sendCancel(AI_CANCEL_KEY); idle(now); }
  }
}

bool AiSession::start(uint8_t reason, uint8_t keyword, uint32_t now) {
  if (!_enabled || _muted) return false;
  if (!_ready || !_rail_up) {  // cannot hear or answer: say so instead of going quiet
    _io.notReady();
    return false;
  }
  if (_s == AiSessionState::Speaking || _s == AiSessionState::WaitingRail) {
    if (reason == AI_LISTEN_WAKE) return false;  // v1: no wake-word barge-in (the card's own voice would trigger it)
    _io.flushSpeech();
    _io.sendSpoken(AI_SPOKEN_ABORTED);
    idle(now);
  } else if (_s != AiSessionState::Idle) {
    return false;
  }
  _io.sendListen(reason, keyword);
  _io.micForward(true);
  enter(AiSessionState::Listening, now);
  return true;
}

void AiSession::cancel(uint8_t reason, uint32_t now) {
  if (_s == AiSessionState::Idle) return;
  if (_s == AiSessionState::Speaking || _s == AiSessionState::WaitingRail) {
    abortSpeech(reason, now);
    return;
  }
  _io.sendCancel(reason);
  idle(now);
}

void AiSession::onListenStop(uint32_t now) {
  if (_s != AiSessionState::Listening) return;
  _io.micForward(false);
  enter(AiSessionState::Thinking, now);
}

void AiSession::onSpeech(uint32_t, uint32_t now) {
  _last_speech_ms = now;
  if (_s == AiSessionState::Thinking || _s == AiSessionState::Listening) {
    _io.micForward(false);
    _io.railRequest();
    enter(AiSessionState::WaitingRail, now);
  }
}

void AiSession::onRail(uint8_t state, uint32_t now) {
  if (state == 1) {
    if (_s == AiSessionState::WaitingRail) {
      _granted = true;
      _io.retEnable(true);
      _io.sendSpoken(AI_SPOKEN_STARTED);
      enter(AiSessionState::Speaking, now);
      _last_speech_ms = now;
    }
  } else {  // denied, or a grant withdrawn
    if (_s == AiSessionState::Speaking) {
      _io.retEnable(false);  // within 10 ms of the withdrawal
      _granted = false;
      abortSpeech(AI_CANCEL_RAIL_LOST, now);
    } else if (_s == AiSessionState::WaitingRail) {
      abortSpeech(AI_CANCEL_NO_RAIL, now);
    }
  }
}

void AiSession::onSpeechEnd(uint32_t now) {
  _speech_end = true;
  if (_s == AiSessionState::Thinking || _s == AiSessionState::Listening) idle(now);  // an answer with no sound
}

void AiSession::onSpeechCancel(uint32_t now) {
  if (_s == AiSessionState::Speaking || _s == AiSessionState::WaitingRail) {
    _io.flushSpeech();
    _io.sendSpoken(AI_SPOKEN_ABORTED);
    idle(now);
  }
}

void AiSession::onSpeechDrained(uint32_t now) {
  if (_s == AiSessionState::Speaking && _speech_end) {
    _io.sendSpoken(AI_SPOKEN_DONE);
    idle(now);
  }
}

void AiSession::onAssistantIdle(uint32_t now) {
  if (_s == AiSessionState::Thinking) idle(now);
}

void AiSession::poll(uint32_t now) {
  switch (_s) {
    case AiSessionState::Idle: break;
    case AiSessionState::Listening:
      if (now - _t0 >= kListenMaxMs) { _io.sendCancel(AI_CANCEL_TIMEOUT); idle(now); }
      break;
    case AiSessionState::Thinking:
      if (now - _t0 >= kThinkMaxMs) { _io.sendCancel(AI_CANCEL_TIMEOUT); idle(now); }
      break;
    case AiSessionState::WaitingRail:
      if (now - _t0 >= kRailWaitMs) abortSpeech(AI_CANCEL_NO_RAIL, now);
      break;
    case AiSessionState::Speaking:
      if (!_speech_end && now - _last_speech_ms >= kCreditSilenceMs) abortSpeech(AI_CANCEL_TIMEOUT, now);
      break;
  }
}
