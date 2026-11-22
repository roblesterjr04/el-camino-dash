#pragma once

// The AI link (docs/protocols/ai_link.md): COBS-framed, CRC-16 protected messages on the UART between the card's ESP32-S3
// and the compute module. No Arduino or ESP-IDF dependencies: host-tested (test/host/run_tests.sh).

#include <stddef.h>
#include <stdint.h>

namespace ailink {

// ESP32 -> module
enum : uint8_t {
  T_HELLO = 0x01, T_PING = 0x02, T_SHUTDOWN = 0x03, T_SETTINGS = 0x04,
  T_LISTEN = 0x10, T_AUDIO = 0x11, T_CANCEL = 0x12, T_SPOKEN = 0x13,
  T_CAN_RX = 0x20, T_CAN_NODES = 0x21, T_CTX = 0x22,
  T_SPEECH_CREDIT = 0x31, T_CAN_TX_RESULT = 0x30,
};
// module -> ESP32
enum : uint8_t {
  T_READY = 0x81, T_PONG = 0x82, T_SHUTDOWN_ACK = 0x83,
  T_STATE = 0x90, T_LISTEN_STOP = 0x91, T_TEXT = 0x92,
  T_SPEECH = 0xA0, T_SPEECH_END = 0xA1, T_SPEECH_CANCEL = 0xA2,
  T_CAN_TX = 0xB0, T_SUBSCRIBE = 0xB1, T_CTX_REQUEST = 0xB2, T_LOG = 0xF0,
};

static const size_t kMaxPayload = 1024;
static const size_t kMaxBody = 1 + 1 + kMaxPayload + 2;       // type, seq, payload, crc
static const size_t kMaxWire = kMaxBody + kMaxBody / 254 + 3;  // COBS overhead, delimiter

/** COBS-encodes in[0..n) (no delimiter written). Returns the encoded length, 0 when cap is too small. */
size_t cobs_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
/** Decodes one COBS block (no delimiter). Returns the decoded length, or 0 on a malformed block / small cap. */
size_t cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

/** One complete wire frame (leading nothing, trailing 0x00) for a message. Returns its length, 0 if it did not fit. */
size_t build_frame(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

struct Frame {
  uint8_t type = 0;
  uint8_t seq = 0;
  const uint8_t *payload = nullptr;  // valid until the next Rx::feed()
  size_t len = 0;
};

/** Byte-stream parser: feed it bytes as they arrive. */
class Rx {
 public:
  /** True when this byte completed a good frame (in out). */
  bool feed(uint8_t b, Frame &out);
  uint32_t frames() const { return _frames; }
  uint32_t bad() const { return _bad; }      // bad CRC / COBS / too long
  uint32_t gaps() const { return _gaps; }    // missing sequence numbers

 private:
  uint8_t _in[kMaxWire];
  uint8_t _body[kMaxBody];
  size_t _n = 0;
  bool _overflow = false;
  bool _have_seq = false;
  uint8_t _next_seq = 0;
  uint32_t _frames = 0, _bad = 0, _gaps = 0;
};

/** Sender side: owns the sequence number. */
class Tx {
 public:
  size_t frame(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out, size_t cap) {
    return build_frame(type, _seq++, payload, len, out, cap);
  }

 private:
  uint8_t _seq = 0;
};

// ---- payload helpers (little-endian) ----
struct Put {
  uint8_t *p;
  size_t cap, n = 0;
  Put(uint8_t *buf, size_t c) : p(buf), cap(c) {}
  Put &u8(uint8_t v) { if (n < cap) p[n] = v; n++; return *this; }
  Put &u16(uint16_t v) { return u8((uint8_t)v).u8((uint8_t)(v >> 8)); }
  Put &u32(uint32_t v) { return u16((uint16_t)v).u16((uint16_t)(v >> 16)); }
  Put &bytes(const void *b, size_t k) { for (size_t i = 0; i < k; i++) u8(((const uint8_t *)b)[i]); return *this; }
  bool ok() const { return n <= cap; }
};
struct Get {
  const uint8_t *p;
  size_t n, i = 0;
  bool bad = false;
  Get(const uint8_t *buf, size_t len) : p(buf), n(len) {}
  uint8_t u8() { if (i < n) return p[i++]; bad = true; return 0; }
  uint16_t u16() { const uint16_t lo = u8(); return (uint16_t)(lo | (uint16_t)u8() << 8); }
  uint32_t u32() { const uint32_t lo = u16(); return lo | (uint32_t)u16() << 16; }
  size_t left() const { return n - i; }
  const uint8_t *rest() const { return p + i; }
};

}  // namespace ailink
