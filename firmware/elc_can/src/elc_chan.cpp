#include "elc_chan.h"

#include <stdio.h>
#include <string.h>

static const uint32_t kGapMs = 100;      // more between frames of one transfer drops it
static const uint32_t kDupMs = 1000;     // the same transfer ID again within this is a duplicate

uint16_t elc_ch_base(uint16_t id) {
  if (id >= 0x400) return 0;
  const uint16_t base = id & ~0x1F;
  switch (base) {
    case ELC_CH_ALERT: case ELC_CH_INPUT: case ELC_CH_SYSTEM: case ELC_CH_SENSOR: case ELC_CH_AUDIO_CMD:
    case ELC_CH_UI: case ELC_CH_STORE: case ELC_CH_BULK: case ELC_CH_ONLINE: case ELC_CH_NET: case ELC_CH_LOG:
      return base;
    default:
      return 0;
  }
}

uint16_t elc_ch_crc16(const uint8_t *p, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

size_t elc_ch_frames(uint16_t id, const uint8_t *payload, size_t len, uint8_t tid, ElcCanFrame *out, size_t max) {
  if (len < 1 || len > ELC_CH_MAX_PAYLOAD || !out || max == 0) return 0;
  tid &= 0x1F;
  if (len <= 7) {
    out[0].id = id;
    memcpy(out[0].data, payload, len);
    out[0].data[len] = (uint8_t)(0xC0 | tid);
    out[0].len = (uint8_t)(len + 1);
    return 1;
  }
  const size_t total = len + 2;
  const size_t frames = (total + 6) / 7;
  if (frames > max) return 0;
  const uint16_t crc = elc_ch_crc16(payload, len);
  for (size_t k = 0; k < frames; k++) {
    ElcCanFrame &f = out[k];
    f.id = id;
    size_t n = 0;
    for (size_t i = k * 7; i < total && n < 7; i++, n++) {
      f.data[n] = i == 0 ? (uint8_t)(crc & 0xFF) : i == 1 ? (uint8_t)(crc >> 8) : payload[i - 2];
    }
    f.data[n] = (uint8_t)((k == 0 ? 0x80 : 0) | (k + 1 == frames ? 0x40 : 0) | ((k & 1) << 5) | tid);
    f.len = (uint8_t)(n + 1);
  }
  return frames;
}

bool ElcChRx::complete(uint16_t id, uint8_t tid, const uint8_t *p, size_t n, uint32_t now_ms, ElcChTransfer &out) {
  Dup *d = nullptr;
  for (Dup &x : _dup) {
    if (x.used && x.id == id) d = &x;
  }
  if (d && d->tid == tid && now_ms - d->ms < kDupMs) return false;  // a retried copy of the last one
  if (!d) {
    d = &_dup[0];
    for (Dup &x : _dup) {
      if (!x.used) {
        d = &x;
        break;
      }
      if ((int32_t)(x.ms - d->ms) < 0) d = &x;
    }
  }
  d->used = true;
  d->id = id;
  d->tid = tid;
  d->ms = now_ms;
  out.base = (uint16_t)(id & ~0x1F);
  out.src = (uint8_t)(id & 0x1F);
  out.data = p;
  out.len = n;
  return true;
}

bool ElcChRx::feed(const ElcCanFrame &f, uint32_t now_ms, ElcChTransfer &out) {
  if (f.len < 1 || f.len > 8) return false;
  const uint8_t tail = f.data[f.len - 1];
  const bool sot = tail & 0x80, eot = tail & 0x40;
  const uint8_t tog = (tail >> 5) & 1, tid = tail & 0x1F;
  const size_t body = f.len - 1u;
  Buf *b = nullptr;
  for (Buf &x : _buf) {
    if (x.busy && x.id == f.id) b = &x;
  }
  if (sot) {
    if (b) {
      b->busy = false;  // a new transfer drops an unfinished one
      _dropped++;
    }
    if (tog) {
      _dropped++;
      return false;
    }
    if (eot) {
      if (body == 0) return false;
      memcpy(_single, f.data, body);
      return complete(f.id, tid, _single, body, now_ms, out);
    }
    Buf *use = nullptr;
    for (Buf &x : _buf) {
      if (!x.busy) {
        use = &x;
        break;
      }
    }
    if (!use) {  // all busy: recycle the oldest
      use = &_buf[0];
      for (Buf &x : _buf) {
        if ((int32_t)(x.ms - use->ms) < 0) use = &x;
      }
      _dropped++;
    }
    use->busy = true;
    use->id = f.id;
    use->tid = tid;
    use->toggle = 1;
    use->ms = now_ms;
    memcpy(use->data, f.data, body);
    use->n = body;
    return false;
  }
  if (!b) return false;
  if (b->tid != tid || b->toggle != tog || now_ms - b->ms > kGapMs || b->n + body > sizeof(b->data)) {
    b->busy = false;
    _dropped++;
    return false;
  }
  memcpy(b->data + b->n, f.data, body);
  b->n += body;
  b->ms = now_ms;
  b->toggle ^= 1;
  if (!eot) return false;
  b->busy = false;
  if (b->n < 3 || (uint16_t)(b->data[0] | b->data[1] << 8) != elc_ch_crc16(b->data + 2, b->n - 2)) {
    _dropped++;
    return false;
  }
  return complete(f.id, tid, b->data + 2, b->n - 2, now_ms, out);
}

// ---- payload helpers

ElcChWriter &ElcChWriter::u8(uint8_t v) {
  if (_n < sizeof(_b)) {
    _b[_n++] = v;
  } else {
    _over = true;
  }
  return *this;
}
ElcChWriter &ElcChWriter::u16(uint16_t v) { return u8((uint8_t)v).u8((uint8_t)(v >> 8)); }
ElcChWriter &ElcChWriter::u32(uint32_t v) { return u16((uint16_t)v).u16((uint16_t)(v >> 16)); }
ElcChWriter &ElcChWriter::bytes(const void *p, size_t n) {
  const uint8_t *q = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) u8(q[i]);
  return *this;
}
ElcChWriter &ElcChWriter::text(const char *s, size_t max) {
  for (size_t i = 0; s && s[i] && i < max; i++) u8((uint8_t)s[i]);
  return *this;
}

uint8_t ElcChReader::u8() {
  if (_i >= _n) {
    _short = true;
    return 0;
  }
  return _p[_i++];
}
uint16_t ElcChReader::u16() {
  const uint16_t a = u8();
  return (uint16_t)(a | (uint16_t)u8() << 8);
}
uint32_t ElcChReader::u32() {
  const uint32_t a = u16();
  return a | (uint32_t)u16() << 16;
}
size_t ElcChReader::rest_text(char *out, size_t cap) {
  size_t k = 0;
  while (_i < _n) {
    const uint8_t c = _p[_i++];
    if (k + 1 < cap) out[k++] = (char)c;
  }
  if (cap) out[k] = 0;
  return k;
}
size_t ElcChReader::field(char *out, size_t cap) {
  size_t k = 0;
  while (_i < _n) {
    const uint8_t c = _p[_i++];
    if (c == 0) break;
    if (k + 1 < cap) out[k++] = (char)c;
  }
  if (cap) out[k] = 0;
  return k;
}

// ---- NET

size_t elc_net_encode_announce(const ElcNetAnnounce &a, uint8_t *out, size_t cap) {
  ElcChWriter w;
  w.u8(ELC_NET_ANNOUNCE).u8(a.version).u8(a.type).u16(a.product).u16(a.fw).u32(a.uid).u8(a.generation)
      .u32(a.produces).u32(a.consumes).u8(a.flags).text(a.name, 24);
  if (!w.ok() || w.size() > cap) return 0;
  memcpy(out, w.data(), w.size());
  return w.size();
}

bool elc_net_decode_announce(const uint8_t *p, size_t n, ElcNetAnnounce &out) {
  ElcChReader r(p, n);
  if (r.u8() != ELC_NET_ANNOUNCE) return false;
  out.version = r.u8();
  out.type = r.u8();
  out.product = r.u16();
  out.fw = r.u16();
  out.uid = r.u32();
  out.generation = r.u8();
  out.produces = r.u32();
  out.consumes = r.u32();
  out.flags = r.u8();
  if (!r.ok()) return false;
  r.rest_text(out.name, sizeof(out.name));
  return true;
}

size_t elc_net_encode_rail(const ElcNetRail &rl, uint8_t *out, size_t cap) {
  if (cap < 6) return 0;
  ElcChWriter w;
  w.u8(ELC_NET_RAIL).u8(rl.dest).u8(rl.slot).u8(rl.state).u16(rl.rate10);
  memcpy(out, w.data(), w.size());
  return w.size();
}

bool elc_net_decode_rail(const uint8_t *p, size_t n, ElcNetRail &out) {
  ElcChReader r(p, n);
  if (r.u8() != ELC_NET_RAIL) return false;
  out.dest = r.u8();
  out.slot = r.u8();
  out.state = r.u8();
  out.rate10 = r.u16();
  return r.ok();
}

// ---- the service

void ElcChannels::begin(uint8_t node, const ElcNetAnnounce &me, SendFn send, void *send_ctx) {
  _node = node & 0x1F;
  _me = me;
  _me.produces |= elc_ch_bit(ELC_CH_NET);
  _me.consumes |= elc_ch_bit(ELC_CH_NET);
  _send = send;
  _send_ctx = send_ctx;
  _booted = false;
}

bool ElcChannels::send(uint16_t base, const uint8_t *payload, size_t len) {
  if (!_send) return false;
  ElcCanFrame fr[ELC_CH_MAX_FRAMES];
  uint8_t &tid = _tid[(base >> 5) & 31];
  const size_t n = elc_ch_frames((uint16_t)(base + _node), payload, len, tid, fr, ELC_CH_MAX_FRAMES);
  if (!n) return false;
  tid = (uint8_t)((tid + 1) & 0x1F);
  bool ok = true;
  for (size_t i = 0; i < n; i++) ok = _send(fr[i], _send_ctx) && ok;
  if (ok) {
    _out++;
  } else {
    _send_fail++;
  }
  return ok;
}

void ElcChannels::announce() {
  uint8_t b[64];
  const size_t n = elc_net_encode_announce(_me, b, sizeof(b));
  if (n) send(ELC_CH_NET, b, n);
}

void ElcChannels::query(uint8_t dest, uint8_t what) {
  const uint8_t b[3] = {ELC_NET_QUERY, dest, what};
  send(ELC_CH_NET, b, sizeof(b));
}

void ElcChannels::descriptionsChanged() {
  _me.generation = (uint8_t)(_me.generation % 255 + 1);
  if (_booted) announce();
}

void ElcChannels::setFactoryDefaults(bool on) {
  const uint8_t f = (uint8_t)((_me.flags & ~1) | (on ? 1 : 0));
  if (f == _me.flags) return;
  _me.flags = f;
  if (_booted) announce();
}

void ElcChannels::setSlot(uint8_t slot) { _me.flags = (uint8_t)((_me.flags & ~0x0E) | ((slot & 7) << 1)); }

void ElcChannels::feed(const ElcCanFrame &f, uint32_t now_ms) {
  if (f.id >= 0x700 && f.id < 0x720 && f.len >= 8 && (f.data[7] & 1)) {  // a channel node's heartbeat
    const uint8_t n = (uint8_t)(f.id - 0x700);
    if (n != _node && n && !_nodes[n].announced && (!_asked_ms[n] || now_ms - _asked_ms[n] >= 5000)) {
      _asked_ms[n] = now_ms ? now_ms : 1;
      _ask_mask |= 1u << n;
    }
    return;
  }
  if (!elc_ch_base(f.id) || elc_ch_src(f.id) == _node) return;
  ElcChTransfer t;
  if (!_rx.feed(f, now_ms, t)) return;
  _in++;
  if (t.base == ELC_CH_NET) {
    const uint8_t op = t.op();
    if (op == ELC_NET_ANNOUNCE) {
      ElcNetAnnounce a;
      if (elc_net_decode_announce(t.data, t.len, a)) {
        Node &nd = _nodes[t.src & 0x1F];
        nd.announced = true;
        nd.ms = now_ms ? now_ms : 1;
        nd.a = a;
      }
    } else if (t.for_me(_node) && t.len >= 3) {
      if (op == ELC_NET_QUERY) {
        // answer after (node x 8) ms so everyone does not talk at once; queries meanwhile merge
        if (!_pending_what) _pending_at = now_ms + (uint32_t)_node * 8u;
        _pending_what |= t.data[2];
      } else if ((op == ELC_NET_IDENTIFY || op == ELC_NET_RESTART) && _on_net) {
        _on_net(op, t.data, t.len, t.src, _on_net_ctx);
      }
    }
  }
  if (_on_transfer) _on_transfer(t, _on_transfer_ctx);
}

void ElcChannels::poll(uint32_t now_ms) {
  if (!_send) return;
  if (!_booted) {
    _booted = true;
    announce();
    if (_on_describe) _on_describe(ELC_NET_Q_ALL & ~ELC_NET_Q_ANNOUNCE, _on_describe_ctx);
    return;
  }
  for (uint8_t n = 1; _ask_mask && n < 32; n++) {
    if (_ask_mask & (1u << n)) {
      _ask_mask &= ~(1u << n);
      query(n, ELC_NET_Q_ALL);  // its ANNOUNCE and descriptions
    }
  }
  if (_pending_what && (int32_t)(now_ms - _pending_at) >= 0) {
    const uint8_t what = _pending_what;
    _pending_what = 0;
    if (what & ELC_NET_Q_ANNOUNCE) announce();
    if ((what & ~ELC_NET_Q_ANNOUNCE) && _on_describe) _on_describe(what & ~ELC_NET_Q_ANNOUNCE, _on_describe_ctx);
  }
}

uint8_t ElcChannels::findNode(uint8_t type, uint16_t product) const {
  for (uint8_t i = 1; i < 32; i++) {
    const Node &n = _nodes[i];
    if (n.announced && n.a.type == type && (!product || n.a.product == product)) return i;
  }
  return 0;
}

uint8_t ElcChannels::findConsumer(uint16_t base) const {
  for (uint8_t i = 1; i < 32; i++) {
    if (_nodes[i].announced && (_nodes[i].a.consumes & elc_ch_bit(base))) return i;
  }
  return 0;
}

void ElcChannels::describe(char *out, size_t n) const {
  if (!out || !n) return;
  int w = snprintf(out, n, "channels: in %lu out %lu dropped %lu; announced:", (unsigned long)_in,
                   (unsigned long)_out, (unsigned long)dropped());
  bool any = false;
  for (uint8_t i = 0; i < 32 && w > 0 && (size_t)w < n; i++) {
    if (!_nodes[i].announced) continue;
    w += snprintf(out + w, n - (size_t)w, " 0x%02x %s (type %u, product 0x%04x, slot %u)", i, _nodes[i].a.name,
                  (unsigned)_nodes[i].a.type, (unsigned)_nodes[i].a.product, (unsigned)_nodes[i].a.slot());
    any = true;
  }
  if (!any && w > 0 && (size_t)w < n) snprintf(out + w, n - (size_t)w, " none");
}
