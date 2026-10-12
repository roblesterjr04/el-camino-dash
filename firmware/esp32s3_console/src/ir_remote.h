#pragma once

// IR remotes on a 38 kHz IR demodulator (a TSOP38438 on J6, IR_IN): the Apple remote (the white plastic one and the
// aluminum one) and the old Sirius radio remote. No Arduino code: the board's interrupt hands it the line's levels and
// how long each lasted, so it runs in the host tests too.
//
// Both speak NEC: a 9 ms mark, a 4.5 ms space, 32 bits sent LSB first (each a 560 us mark, then a 560 us space for 0 or
// 1690 us for 1) and a closing mark; while a button stays down, a repeat (9 ms mark, 2.25 ms space, mark) every 108 ms.
// - Apple: 0xEE 0x87, the command, and the remote's pairing ID (any remote is taken). Bit 0 of the command differs
//   between remotes, so the button is the command shifted right one.
// - Sirius: address 0x01 and its inverse, the command and its inverse (codes read off one on the bench, 2026-10-11).

#include <stdint.h>

enum class RemoteKey : uint8_t {
  None, Up, Down, Left, Right, Select, Menu, Play, VolUp, VolDown, Mute, Band, Display, Power,
  Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,  // in order: Digit1 + n - 1 is n
  Sel,  // the Sirius SEL: OK where there is something to pick, play / pause at rest
  kCount
};

/** NEC frames and repeats out of the demodulator's marks (carrier on: the TSOP's output low) and spaces. */
class NecDecoder {
 public:
  enum Result : uint8_t { kNothing, kFrame, kRepeat };
  /** One level that has just ended: `mark` true for a mark, its length in us. A frame's 32 bits land in `code`. */
  Result feed(bool mark, uint32_t us, uint32_t *code);

 private:
  enum State : uint8_t { kIdle, kLeadSpace, kBitMark, kBitSpace, kRepeatMark };
  State _st = kIdle;
  uint8_t _n = 0;
  uint32_t _bits = 0;
};

/** The button for a frame's 32 bits, or None for another remote's. */
RemoteKey remote_key(uint32_t code);
const char *remote_key_name(RemoteKey k);

struct RemoteEvents {
  RemoteKey press = RemoteKey::None;  // went down: Up / Down / Vol + / Vol - step at once
  RemoteKey step = RemoteKey::None;   // those held: one more step (auto-repeat)
  RemoteKey click = RemoteKey::None;  // the others: let go before the hold time
  RemoteKey hold = RemoteKey::None;   // the others: held for the hold time (once; letting go then does nothing)
};

/** Turns frames and repeats into presses, clicks and holds. The remote never says when a button is let go: it is up
 *  once no repeat has come for kReleaseMs. */
class IrRemote {
 public:
  static constexpr uint32_t kReleaseMs = 200;      // repeats come every 108 ms
  static constexpr uint32_t kRepeatDelayMs = 450;  // Up / Down / Vol: the first auto-repeat step
  static constexpr uint32_t kRepeatMs = 100;       // then a step per repeat (one each 108 ms)

  explicit IrRemote(uint32_t hold_ms) : _hold_ms(hold_ms) {}
  void frame(uint32_t code, uint32_t now_ms);
  void repeat(uint32_t now_ms);
  /** What happened since the last call. Call every loop pass. */
  void poll(RemoteEvents &ev, uint32_t now_ms);
  RemoteKey down() const { return _key; }

 private:
  static bool steps(RemoteKey k) {
    return k == RemoteKey::Up || k == RemoteKey::Down || k == RemoteKey::VolUp || k == RemoteKey::VolDown;
  }
  void release(uint32_t now_ms);
  uint32_t _hold_ms;
  RemoteKey _key = RemoteKey::None;
  bool _alu_center = false;  // the aluminum remote's centre or play, which may follow with the white remote's select
  uint32_t _down_ms = 0, _seen_ms = 0, _step_ms = 0;
  bool _held = false;
  RemoteEvents _ev;
};
