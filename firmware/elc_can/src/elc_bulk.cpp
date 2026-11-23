#include "elc_bulk.h"

#include <string.h>

uint32_t elc_crc32(const uint8_t *p, size_t n, uint32_t crc) {
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
  }
  return ~crc;
}

// ---- codecs ----

size_t elc_bulk_encode(const ElcBulkInfo &m, uint8_t *out, size_t cap) {
  ElcChWriter w;
  w.u8(ELC_BULK_INFO).u8(m.to).u8(m.tag).u8(m.session).u32(m.size).u32(m.crc);
  if (m.session == ELC_BULK_REFUSED) w.u8(m.why);
  if (!w.ok() || w.size() > cap) return 0;
  memcpy(out, w.data(), w.size());
  return w.size();
}

size_t elc_bulk_encode(const ElcBulkWrite &m, uint8_t *out, size_t cap) {
  ElcChWriter w;
  w.u8(ELC_BULK_WRITE).u8(m.dest).u8(m.tag).u8(m.kind).u16(m.object).u32(m.size).u32(m.crc).u8(m.session & 7);
  if (m.kind == ELC_BULK_KIND_FIRMWARE) w.u16(m.fw).text(m.variant, 32);
  if (!w.ok() || w.size() > cap) return 0;
  memcpy(out, w.data(), w.size());
  return w.size();
}

size_t elc_bulk_encode(const ElcBulkWant &m, uint8_t *out, size_t cap) {
  if (cap < 8) return 0;
  out[0] = ELC_BULK_WANT;
  out[1] = m.dest;
  out[2] = m.session & 7;
  out[3] = (uint8_t)m.first;
  out[4] = (uint8_t)(m.first >> 8);
  out[5] = (uint8_t)(m.first >> 16);
  out[6] = m.credit;
  return 7;
}

size_t elc_bulk_encode(const ElcBulkEnd &m, uint8_t *out, size_t cap) {
  if (cap < 4) return 0;
  out[0] = ELC_BULK_END;
  out[1] = m.dest;
  out[2] = m.session & 7;
  out[3] = m.result;
  return 4;
}

bool elc_bulk_decode(const uint8_t *p, size_t n, ElcBulkInfo &out) {
  if (n < 12 || p[0] != ELC_BULK_INFO) return false;
  ElcChReader r(p + 1, n - 1);
  out.to = r.u8();
  out.tag = r.u8();
  out.session = r.u8();
  out.size = r.u32();
  out.crc = r.u32();
  out.why = r.left() ? r.u8() : (uint8_t)ELC_BULK_WHY_NONE;
  return r.ok();
}

bool elc_bulk_decode(const uint8_t *p, size_t n, ElcBulkWrite &out) {
  if (n < 15 || p[0] != ELC_BULK_WRITE) return false;
  ElcChReader r(p + 1, n - 1);
  out = ElcBulkWrite();
  out.dest = r.u8();
  out.tag = r.u8();
  out.kind = r.u8();
  out.object = r.u16();
  out.size = r.u32();
  out.crc = r.u32();
  out.session = r.left() ? (uint8_t)(r.u8() & 7) : 0;
  if (out.kind == ELC_BULK_KIND_FIRMWARE && r.left() >= 2) {
    out.fw = r.u16();
    r.rest_text(out.variant, sizeof(out.variant));
  }
  return r.ok();
}

bool elc_bulk_decode(const uint8_t *p, size_t n, ElcBulkWant &out) {
  if (n < 7 || p[0] != ELC_BULK_WANT) return false;
  out.dest = p[1];
  out.session = p[2] & 7;
  out.first = (uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16);
  out.credit = p[6];
  return true;
}

bool elc_bulk_decode(const uint8_t *p, size_t n, ElcBulkEnd &out) {
  if (n < 4 || p[0] != ELC_BULK_END) return false;
  out.dest = p[1];
  out.session = p[2] & 7;
  out.result = p[3];
  return true;
}

void elc_bulk_data_frame(uint8_t node, uint8_t session, uint32_t chunk, const uint8_t *p, size_t n, ElcCanFrame &out) {
  if (n > ELC_BULK_CHUNK_BYTES) n = ELC_BULK_CHUNK_BYTES;
  out.id = (uint16_t)(ELC_CH_BULK_DATA + (node & 0x1F));
  const uint32_t h = ((uint32_t)(session & 7) << 21) | (chunk & 0x1FFFFF);
  out.data[0] = (uint8_t)h;
  out.data[1] = (uint8_t)(h >> 8);
  out.data[2] = (uint8_t)(h >> 16);
  memcpy(out.data + 3, p, n);
  out.len = (uint8_t)(3 + n);
}

bool elc_bulk_data_decode(const ElcCanFrame &f, uint8_t *session, uint32_t *chunk, const uint8_t **p, size_t *n) {
  if ((f.id & ~0x1F) != ELC_CH_BULK_DATA || f.len < 4 || f.len > 8) return false;
  const uint32_t h = (uint32_t)f.data[0] | ((uint32_t)f.data[1] << 8) | ((uint32_t)f.data[2] << 16);
  *session = (uint8_t)(h >> 21);
  *chunk = h & 0x1FFFFF;
  *p = f.data + 3;
  *n = (size_t)(f.len - 3);
  return true;
}

static bool bulk_send(ElcChannels *ch, const uint8_t *b, size_t n) { return ch && n && ch->send(ELC_CH_BULK, b, n); }

// ---- writer ----

void ElcBulkWriter::begin(ElcChannels *ch, RawSendFn raw, void *raw_ctx) {
  _ch = ch;
  _raw = raw;
  _raw_ctx = raw_ctx;
  if (ch) ch->addService(this);
}

bool ElcBulkWriter::start(const ElcBulkWrite &w, ReadFn read, void *read_ctx, uint32_t now_ms) {
  if (busy() || !_ch || !_raw || !read || w.size == 0 || w.size > ELC_BULK_MAX_SIZE) return false;
  _w = w;
  _w.tag = _tag++;
  _session = (uint8_t)((_session + 1) & 7);
  _w.session = _session;
  _read = read;
  _read_ctx = read_ctx;
  _chunks = elc_bulk_chunks(w.size);
  _n = 0;
  _frames = _resends = _high = 0;
  _tokens_x10 = 0;
  _write_tries = 0;
  _offer_ms = _last_tick = now_ms;
  _state = State::Offering;
  send_write(now_ms);
  return true;
}

void ElcBulkWriter::send_write(uint32_t now_ms) {
  uint8_t b[ELC_CH_MAX_PAYLOAD];
  bulk_send(_ch, b, elc_bulk_encode(_w, b, sizeof(b)));
  _write_ms = now_ms;
  _write_tries++;
}

ElcBulkWriter::Receiver *ElcBulkWriter::find(uint8_t node) {
  for (int i = 0; i < _n; i++) {
    if (_rx[i].node == node) return &_rx[i];
  }
  return nullptr;
}

void ElcBulkWriter::transfer(const ElcChTransfer &t, uint32_t now_ms) {
  if (t.base != ELC_CH_BULK || !busy() || !_ch) return;
  const uint8_t me = _ch->node();
  if (_w.dest && t.src != _w.dest) return;
  Receiver *r = find(t.src);
  switch (t.op()) {
    case ELC_BULK_INFO: {
      ElcBulkInfo m;
      if (!elc_bulk_decode(t.data, t.len, m) || m.to != me || m.tag != _w.tag) return;
      if (!r) {
        if (_n >= kMaxReceivers) return;
        r = &_rx[_n++];
        *r = Receiver();
        r->node = t.src;
        r->state = m.session == ELC_BULK_REFUSED ? RxState::Refused : RxState::Accepted;
        r->result = m.why;
        // a late taker of a multicast that already moved on cannot have the start any more
        if (r->state == RxState::Accepted && lowWater() > 0 && _state == State::Sending) {
          r->state = RxState::Failed;
          r->result = ELC_BULK_ABORTED;
        }
      }
      r->heard_ms = now_ms;
      if (_w.dest && r->state == RxState::Accepted) _state = State::Sending;
      if (_w.dest && r->state == RxState::Refused) finish();  // the one node asked said no
      return;
    }
    case ELC_BULK_WANT: {
      ElcBulkWant m;
      if (!elc_bulk_decode(t.data, t.len, m) || m.dest != me || m.session != _w.session) return;
      if (!r) {
        // its INFO was lost: take it on while the start of the object is still to hand
        if (_n >= kMaxReceivers || m.first != 0 || (_state == State::Sending && lowWater() > 0)) return;
        r = &_rx[_n++];
        *r = Receiver();
        r->node = t.src;
      }
      if (r->state != RxState::Accepted) return;
      r->heard_ms = now_ms;
      if (m.first > _chunks) return;
      // A WANT that moved on only adds credit: frames already on their way past its first are not sent again. One
      // that asks from where it asked last time (it got nothing since: the tail of the window was lost), or a repair
      // (credit 1: one lost chunk, the ones after it held), goes back to its first.
      const bool again = m.first == r->last_first || m.credit == 1 || m.first > r->cursor;
      r->last_first = m.first;
      r->acked = m.first;
      if (again) r->cursor = m.first;
      r->end = m.first + m.credit > _chunks ? _chunks : m.first + m.credit;
      if (_w.dest) _state = State::Sending;
      return;
    }
    case ELC_BULK_END: {
      ElcBulkEnd m;
      if (!elc_bulk_decode(t.data, t.len, m) || m.dest != me || m.session != _w.session || !r) return;
      if (r->state != RxState::Accepted) return;
      r->state = m.result == ELC_BULK_OK ? RxState::Done : RxState::Failed;
      r->result = m.result;
      if (m.result == ELC_BULK_OK) r->acked = r->cursor = _chunks;
      return;
    }
    default: return;
  }
}

uint32_t ElcBulkWriter::lowWater() const {
  if (_state == State::Offering) return 0;
  uint32_t low = _chunks;
  for (int i = 0; i < _n; i++) {
    if (_rx[i].state == RxState::Accepted && _rx[i].acked < low) low = _rx[i].acked;
  }
  return low;
}

uint16_t ElcBulkWriter::progress() const {
  if (!_chunks) return 0;
  bool any = false;
  uint32_t low = _chunks;
  for (int i = 0; i < _n; i++) {
    if (_rx[i].state == RxState::Accepted) {
      any = true;
      if (_rx[i].acked < low) low = _rx[i].acked;
    }
  }
  if (!any) return succeeded() ? 1000 : 0;
  return (uint16_t)((uint64_t)low * 1000 / _chunks);
}

bool ElcBulkWriter::succeeded() const {
  bool done = false;
  for (int i = 0; i < _n; i++) {
    if (_rx[i].state == RxState::Failed || _rx[i].state == RxState::Accepted) return false;
    if (_rx[i].state == RxState::Done) done = true;
  }
  return done;
}

void ElcBulkWriter::finish() { _state = State::Done; }

void ElcBulkWriter::abort(uint32_t now_ms) {
  (void)now_ms;
  if (!busy()) return;
  ElcBulkEnd e;
  e.dest = _w.dest;
  e.session = _w.session;
  e.result = ELC_BULK_ABORTED;
  uint8_t b[8];
  bulk_send(_ch, b, elc_bulk_encode(e, b, sizeof(b)));
  for (int i = 0; i < _n; i++) {
    if (_rx[i].state == RxState::Accepted) {
      _rx[i].state = RxState::Failed;
      _rx[i].result = ELC_BULK_ABORTED;
    }
  }
  finish();
}

void ElcBulkWriter::poll(uint32_t now_ms) {
  const uint32_t dt = now_ms - _last_tick;
  _last_tick = now_ms;
  if (!busy()) return;

  int accepted = 0;
  for (int i = 0; i < _n; i++) accepted += _rx[i].state == RxState::Accepted;

  if (_state == State::Offering) {
    const bool unicast = _w.dest != 0;
    const uint32_t wait = unicast ? kWriteRetryMs : kCollectMs;
    if (now_ms - _write_ms < wait) return;
    if (accepted) {
      _state = State::Sending;
    } else if (_write_tries < kWriteTries) {
      send_write(now_ms);
      return;
    } else {
      finish();  // nobody took it
      return;
    }
  }

  // give up on receivers that went quiet
  for (int i = 0; i < _n; i++) {
    Receiver &r = _rx[i];
    if (r.state == RxState::Accepted && r.heard_ms && now_ms - r.heard_ms > kSilentMs) {
      r.state = RxState::Failed;
      r.result = ELC_BULK_ABORTED;
    } else if (r.state == RxState::Accepted && !r.heard_ms) {
      r.heard_ms = now_ms;
    }
  }
  accepted = 0;
  for (int i = 0; i < _n; i++) accepted += _rx[i].state == RxState::Accepted;
  if (!accepted) {
    finish();
    return;
  }

  // pace: tokens in tenths of a frame, at most a transmit queue's worth banked
  _tokens_x10 += dt * _pace_x10;
  if (_tokens_x10 > 160) _tokens_x10 = 160;
  uint8_t buf[ELC_BULK_CHUNK_BYTES];
  while (_tokens_x10 >= 10) {
    // the lowest chunk anyone still has credit for: serves the slowest receiver first, and every receiver at that
    // chunk takes the same frame
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < _n; i++) {
      const Receiver &r = _rx[i];
      if (r.state == RxState::Accepted && r.cursor < r.end && r.cursor < c) c = r.cursor;
    }
    if (c == 0xFFFFFFFFu) break;
    const uint32_t off = c * ELC_BULK_CHUNK_BYTES;
    const size_t want = _w.size - off < ELC_BULK_CHUNK_BYTES ? (size_t)(_w.size - off) : ELC_BULK_CHUNK_BYTES;
    if (_read(off, buf, want, _read_ctx) < want) break;  // a streaming source not there yet
    ElcCanFrame f;
    elc_bulk_data_frame(_ch->node(), _w.session, c, buf, want, f);
    if (!_raw(f, _raw_ctx)) break;
    _tokens_x10 -= 10;
    _frames++;
    if (c < _high) _resends++;
    if (c + 1 > _high) _high = c + 1;
    for (int i = 0; i < _n; i++) {
      Receiver &r = _rx[i];
      if (r.state == RxState::Accepted && r.cursor == c && r.cursor < r.end) r.cursor++;
    }
  }
}

// ---- receiver ----

void ElcBulkReceiver::begin(ElcChannels *ch, Sink *sink) {
  _ch = ch;
  _sink = sink;
}

void ElcBulkReceiver::want(uint32_t now_ms, uint8_t credit) {
  ElcBulkWant m;
  m.dest = _from;
  m.session = _w.session;
  m.first = _next;
  const uint32_t left = _chunks > _next ? _chunks - _next : 0;
  m.credit = _state == State::Receiving ? (uint8_t)(left < credit ? left : credit) : 0;
  _granted = _next + m.credit;
  uint8_t b[8];
  bulk_send(_ch, b, elc_bulk_encode(m, b, sizeof(b)));
  _ask_ms = now_ms ? now_ms : 1;
  _asks++;
}

void ElcBulkReceiver::end(uint8_t result, uint32_t now_ms) {
  ElcBulkEnd m;
  m.dest = _from;
  m.session = _w.session;
  m.result = result;
  uint8_t b[8];
  bulk_send(_ch, b, elc_bulk_encode(m, b, sizeof(b)));
  _result = result;
  _end_ms = now_ms;
  _state = State::Finished;
}

void ElcBulkReceiver::stop(uint8_t result) {
  _result = result;
  _state = State::Finished;
  _end_repeats = 3;  // nothing more to repeat
  if (_sink) _sink->aborted(result);
}

void ElcBulkReceiver::transfer(const ElcChTransfer &t, uint32_t now_ms) {
  if (t.base != ELC_CH_BULK || !_ch || !_sink) return;
  const uint8_t me = _ch->node();
  if (t.op() == ELC_BULK_WRITE && t.for_me(me)) {
    ElcBulkWrite w;
    if (!elc_bulk_decode(t.data, t.len, w)) return;
    ElcBulkInfo info;
    info.to = t.src;
    info.tag = w.tag;
    info.size = w.size;
    info.crc = w.crc;
    uint8_t b[16];
    if (busy()) {
      // the same WRITE again (our INFO was lost): accept again; anything else waits its turn
      if (t.src == _from && w.tag == _w.tag) {
        info.session = _w.session;
      } else {
        info.session = ELC_BULK_REFUSED;
        info.why = ELC_BULK_WHY_BUSY;
      }
      bulk_send(_ch, b, elc_bulk_encode(info, b, sizeof(b)));
      return;
    }
    const uint8_t why = w.size == 0 || w.size > ELC_BULK_MAX_SIZE ? ELC_BULK_WHY_SIZE : _sink->accept(w, t.src);
    if (why != ELC_BULK_WHY_NONE) {
      // a WRITE to everyone is answered by those who take it, and by those it is meant for that cannot (another build
      // of the same product, busy, not now); nodes that take no such object stay quiet
      if (w.dest != 0 || why == ELC_BULK_WHY_TARGET || why == ELC_BULK_WHY_BUSY || why == ELC_BULK_WHY_NOT_NOW) {
        info.session = ELC_BULK_REFUSED;
        info.why = why;
        bulk_send(_ch, b, elc_bulk_encode(info, b, sizeof(b)));
      }
      return;
    }
    _w = w;
    _from = t.src;
    _chunks = elc_bulk_chunks(w.size);
    _next = _granted = _bytes = 0;
    _crc = 0;
    _crc_from = _sink->crcFrom();
    _result = 0;
    _end_repeats = 0;
    _asks = 0;
    for (bool &h : _held_ok) h = false;
    _state = State::Receiving;
    _heard_ms = _progress_ms = now_ms;
    info.session = w.session;
    bulk_send(_ch, b, elc_bulk_encode(info, b, sizeof(b)));
    want(now_ms);
    return;
  }
  if (t.op() == ELC_BULK_END && busy() && t.src == _from) {
    ElcBulkEnd m;
    if (elc_bulk_decode(t.data, t.len, m) && (m.dest == me || m.dest == 0) && m.session == _w.session) {
      stop(m.result == ELC_BULK_OK ? ELC_BULK_ABORTED : m.result);
    }
  }
}

bool ElcBulkReceiver::take(const uint8_t *p, size_t n, uint32_t now_ms) {
  const uint32_t off = _next * ELC_BULK_CHUNK_BYTES;
  const uint8_t r = _sink->write(off, p, n);
  if (r != ELC_BULK_OK) {
    _sink->aborted(r);
    end(r, now_ms);
    _end_repeats = 1;
    return false;
  }
  // the CRC from crcFrom() on
  if (off + n > _crc_from) {
    const uint32_t skip = off < _crc_from ? _crc_from - off : 0;
    _crc = elc_crc32(p + skip, n - skip, _crc);
  }
  _bytes += (uint32_t)n;
  _next++;
  _progress_ms = now_ms;
  return true;
}

void ElcBulkReceiver::frame(const ElcCanFrame &f, uint32_t now_ms) {
  if (_state != State::Receiving || f.id != ELC_CH_BULK_DATA + _from) return;
  uint8_t session;
  uint32_t chunk;
  const uint8_t *p;
  size_t n;
  if (!elc_bulk_data_decode(f, &session, &chunk, &p, &n) || session != _w.session || chunk >= _chunks) return;
  _heard_ms = now_ms;
  const uint32_t off = chunk * ELC_BULK_CHUNK_BYTES;
  const size_t expect = _w.size - off < ELC_BULK_CHUNK_BYTES ? (size_t)(_w.size - off) : ELC_BULK_CHUNK_BYTES;
  if (n < expect) return;
  if (chunk != _next) {
    if (chunk > _next && chunk - _next < kCredit) {
      // past a lost frame: keep it, and ask for the lost one alone
      const uint32_t k = chunk % kCredit;
      memcpy(_held[k], p, expect);
      _held_chunk[k] = chunk;
      _held_ok[k] = true;
      if (now_ms - _ask_ms >= kGapAskMs) want(now_ms, 1);
    }
    return;
  }
  if (!take(p, expect, now_ms)) return;
  // and whatever was held behind it
  while (_next < _chunks) {
    const uint32_t k = _next % kCredit;
    if (!_held_ok[k] || _held_chunk[k] != _next) break;
    _held_ok[k] = false;
    const uint32_t o = _next * ELC_BULK_CHUNK_BYTES;
    const size_t e = _w.size - o < ELC_BULK_CHUNK_BYTES ? (size_t)(_w.size - o) : ELC_BULK_CHUNK_BYTES;
    if (!take(_held[k], e, now_ms)) return;
  }
  if (_next >= _chunks) {
    const uint8_t res = _sink->finish(_crc == _w.crc);
    if (res == ELC_BULK_PENDING) {
      _state = State::Finishing;
      want(now_ms);  // credit 0: everything is here
    } else {
      end(res, now_ms);
      _end_repeats = 1;
    }
    return;
  }
  if (_next >= _granted || _granted - _next <= kCredit / 2) want(now_ms);
}

void ElcBulkReceiver::poll(uint32_t now_ms) {
  switch (_state) {
    case State::Receiving:
      if (now_ms - _heard_ms >= kGiveUpMs) {
        stop(ELC_BULK_ABORTED);
        ElcBulkEnd m;
        m.dest = _from;
        m.session = _w.session;
        m.result = ELC_BULK_ABORTED;
        uint8_t b[8];
        bulk_send(_ch, b, elc_bulk_encode(m, b, sizeof(b)));
      } else if (now_ms - _progress_ms >= kQuietMs && now_ms - _ask_ms >= kQuietMs) {
        want(now_ms);
      }
      break;
    case State::Finishing: {
      const uint8_t res = _sink->finishing();
      if (res != ELC_BULK_PENDING) {
        end(res, now_ms);
        _end_repeats = 1;
      } else if (now_ms - _ask_ms >= kKeepAliveMs) {
        want(now_ms);
      }
      break;
    }
    case State::Finished:
      // END is a single transfer: say it three times, 100 ms apart, in case one is lost
      if (_end_repeats < 3 && now_ms - _end_ms >= 100) {
        _end_repeats++;
        const uint8_t res = _result;
        end(res, now_ms);
      }
      break;
    default: break;
  }
}
