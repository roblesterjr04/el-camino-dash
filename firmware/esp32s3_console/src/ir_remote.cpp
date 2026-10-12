#include "ir_remote.h"

static bool within(uint32_t us, uint32_t lo, uint32_t hi) { return us >= lo && us <= hi; }

NecDecoder::Result NecDecoder::feed(bool mark, uint32_t us, uint32_t *code) {
  switch (_st) {
    case kIdle:
      break;
    case kLeadSpace:
      if (!mark && within(us, 3500, 5500)) {  // 4.5 ms: a frame follows
        _n = 0;
        _bits = 0;
        _st = kBitMark;
        return kNothing;
      }
      if (!mark && within(us, 1700, 2800)) {  // 2.25 ms: a repeat
        _st = kRepeatMark;
        return kNothing;
      }
      break;
    case kBitMark:
      if (mark && within(us, 250, 900)) {
        if (_n == 32) {  // the closing mark
          _st = kIdle;
          if (code) *code = _bits;
          return kFrame;
        }
        _st = kBitSpace;
        return kNothing;
      }
      break;
    case kBitSpace:
      if (!mark && within(us, 1200, 2200)) _bits |= 1u << _n;
      else if (mark || !within(us, 250, 900)) break;
      _n++;
      _st = kBitMark;
      return kNothing;
    case kRepeatMark:
      if (mark && within(us, 250, 900)) {
        _st = kIdle;
        return kRepeat;
      }
      break;
  }
  // idle, or something that does not fit (another remote, a missed edge): wait for a 9 ms leader
  _st = mark && within(us, 7000, 11000) ? kLeadSpace : kIdle;
  return kNothing;
}

static uint8_t apple_command(uint32_t code) { return (uint8_t)((code >> 16) & 0xFF) >> 1; }

static RemoteKey apple(uint32_t code) {
  switch (apple_command(code)) {
    case 0x01: return RemoteKey::Menu;    // 0x02 / 0x03
    case 0x02: return RemoteKey::Select;  // 0x04 / 0x05: the white remote's centre (its play / pause)
    case 0x03: return RemoteKey::Right;   // 0x06 / 0x07
    case 0x04: return RemoteKey::Left;    // 0x08 / 0x09
    case 0x05: return RemoteKey::Up;      // 0x0A / 0x0B
    case 0x06: return RemoteKey::Down;    // 0x0C / 0x0D
    case 0x2E: return RemoteKey::Select;  // 0x5C / 0x5D: the aluminum remote's centre
    case 0x2F: return RemoteKey::Play;    // 0x5E / 0x5F: the aluminum remote's play / pause
    default: return RemoteKey::None;
  }
}

static RemoteKey sirius(uint8_t cmd) {
  switch (cmd) {
    case 0x02: return RemoteKey::Up;
    case 0x04: return RemoteKey::Down;
    case 0x03: return RemoteKey::Sel;      // SEL
    case 0x06: return RemoteKey::Left;     // CATEGORY <
    case 0x05: return RemoteKey::Right;    // CATEGORY >
    case 0x08: return RemoteKey::Menu;
    case 0x0A: return RemoteKey::VolUp;    // +
    case 0x0E: return RemoteKey::VolDown;  // -
    case 0x1C: return RemoteKey::Mute;
    case 0x0C: return RemoteKey::Band;
    case 0x01: return RemoteKey::Display;
    case 0x00: return RemoteKey::Power;
    case 0x10: return RemoteKey::Digit1;   // the keypad's rows skip a code: 1-3, 4-6, 7-9
    case 0x11: return RemoteKey::Digit2;
    case 0x12: return RemoteKey::Digit3;
    case 0x14: return RemoteKey::Digit4;
    case 0x15: return RemoteKey::Digit5;
    case 0x16: return RemoteKey::Digit6;
    case 0x18: return RemoteKey::Digit7;
    case 0x19: return RemoteKey::Digit8;
    case 0x1A: return RemoteKey::Digit9;
    default: return RemoteKey::None;       // memo 0x0D, 0 0x1D, prev 0x1E
  }
}

RemoteKey remote_key(uint32_t code) {
  if ((code & 0xFFFF) == 0x87EE) return apple(code);
  const uint8_t cmd = (uint8_t)(code >> 16);
  if ((code & 0xFFFF) == 0xFE01 && (uint8_t)(code >> 24) == (uint8_t)~cmd) return sirius(cmd);
  return RemoteKey::None;
}

const char *remote_key_name(RemoteKey k) {
  static const char *const kNames[] = {"none", "up",   "down", "left", "right", "select", "menu", "play",
                                       "vol+", "vol-", "mute", "band", "display", "power", "1",     "2",      "3",    "4",
                                       "5",    "6",    "7",    "8",    "9",     "sel",    "?"};
  return kNames[(int)k];
}

void IrRemote::frame(uint32_t code, uint32_t now_ms) {
  const RemoteKey k = remote_key(code);
  if (k == RemoteKey::None) return;
  if (_key != RemoteKey::None) {
    // the same button sent again, or the aluminum remote's centre / play followed by the white remote's code for old
    // receivers: still the one press
    if (k == _key || (_alu_center && k == RemoteKey::Select)) {
      _seen_ms = now_ms;
      return;
    }
    release(now_ms);
  }
  const uint8_t c = apple_command(code);
  _alu_center = (code & 0xFFFF) == 0x87EE && (c == 0x2E || c == 0x2F);
  _key = k;
  _down_ms = _seen_ms = _step_ms = now_ms;
  _held = false;
  _ev.press = k;
}

void IrRemote::repeat(uint32_t now_ms) {
  if (_key != RemoteKey::None) _seen_ms = now_ms;
}

void IrRemote::release(uint32_t) {
  if (!steps(_key) && !_held) _ev.click = _key;
  _key = RemoteKey::None;
}

void IrRemote::poll(RemoteEvents &ev, uint32_t now_ms) {
  if (_key != RemoteKey::None) {
    // held for as long as the repeats say, not just since the press: the last one may be the button let go
    const uint32_t held_ms = _seen_ms - _down_ms;
    if (now_ms - _seen_ms > kReleaseMs) {
      release(now_ms);
    } else if (steps(_key)) {
      if (held_ms >= kRepeatDelayMs && now_ms - _step_ms >= kRepeatMs) {
        _ev.step = _key;
        _step_ms = now_ms;
      }
    } else if (!_held && held_ms >= _hold_ms) {
      _held = true;
      _ev.hold = _key;
    }
  }
  ev = _ev;
  _ev = RemoteEvents();
}
