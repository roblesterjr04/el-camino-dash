#include "ai_link.h"

#include <elc_chan.h>  // elc_ch_crc16: the same CRC-16/CCITT-FALSE as the CAN channels

namespace ailink {

size_t cobs_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
  if (cap < 1) return 0;
  size_t o = 1, code_at = 0;
  uint8_t code = 1;
  for (size_t i = 0; i < n; i++) {
    if (in[i] == 0) {
      out[code_at] = code;
      code_at = o;
      if (o >= cap) return 0;
      o++;
      code = 1;
    } else {
      if (o >= cap) return 0;
      out[o++] = in[i];
      if (++code == 0xFF) {
        out[code_at] = code;
        code_at = o;
        if (o >= cap) return 0;
        o++;
        code = 1;
      }
    }
  }
  out[code_at] = code;
  return o;
}

size_t cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
  if (n == 0) return 0;
  size_t i = 0, o = 0;
  while (i < n) {
    const uint8_t code = in[i++];
    if (code == 0) return 0;
    for (uint8_t k = 1; k < code; k++) {
      if (i >= n || o >= cap) return 0;
      if (in[i] == 0) return 0;
      out[o++] = in[i++];
    }
    if (code != 0xFF && i < n) {
      if (o >= cap) return 0;
      out[o++] = 0;
    }
  }
  return o;
}

size_t build_frame(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len, uint8_t *out, size_t cap) {
  if (len > kMaxPayload) return 0;
  uint8_t body[kMaxBody];
  body[0] = type;
  body[1] = seq;
  for (size_t i = 0; i < len; i++) body[2 + i] = payload[i];
  const uint16_t crc = elc_ch_crc16(body, 2 + len);
  body[2 + len] = (uint8_t)crc;
  body[3 + len] = (uint8_t)(crc >> 8);
  const size_t e = cobs_encode(body, 4 + len, out, cap);
  if (e == 0 || e + 1 > cap) return 0;
  out[e] = 0;
  return e + 1;
}

bool Rx::feed(uint8_t b, Frame &out) {
  if (b != 0) {
    if (_n < sizeof(_in)) _in[_n++] = b;
    else _overflow = true;
    return false;
  }
  const size_t n = _n;
  const bool over = _overflow;
  _n = 0;
  _overflow = false;
  if (n == 0 && !over) return false;  // a leading / repeated delimiter
  if (over) { _bad++; return false; }
  const size_t m = cobs_decode(_in, n, _body, sizeof(_body));
  if (m < 4) { _bad++; return false; }
  const uint16_t crc = (uint16_t)(_body[m - 2] | _body[m - 1] << 8);
  if (crc != elc_ch_crc16(_body, m - 2)) { _bad++; return false; }
  if (_have_seq && _body[1] != _next_seq) _gaps += (uint8_t)(_body[1] - _next_seq);
  _have_seq = true;
  _next_seq = (uint8_t)(_body[1] + 1);
  out.type = _body[0];
  out.seq = _body[1];
  out.payload = _body + 2;
  out.len = m - 4;
  _frames++;
  return true;
}

}  // namespace ailink
