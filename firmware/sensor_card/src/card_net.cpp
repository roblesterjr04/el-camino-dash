#include "card_net.h"

#include <string.h>

bool card_wants(uint16_t id) {
  if (id == 0x223 || id == 0x213 || id == 0x701) return true;
  const uint16_t b = elc_ch_base(id);
  return b == ELC_CH_NET || b == ELC_CH_UI || b == ELC_CH_ALERT || b == ELC_CH_SYSTEM;
}

void CardNet::begin(uint8_t node, const ElcNetAnnounce &me, TxFn tx, FreeFn tx_free, const Hooks &hooks) {
  _node = node & 0x1F;
  _me = me;
  _me.produces |= elc_ch_bit(ELC_CH_NET);
  _me.consumes |= elc_ch_bit(ELC_CH_NET);
  _tx = tx;
  _free = tx_free;
  _h = hooks;
  _booted = false;
}

bool CardNet::send(uint16_t base, const uint8_t *payload, size_t len) {
  if (!_tx || len < 1 || len > ELC_CH_MAX_PAYLOAD) return false;
  const size_t need = len <= 7 ? 1 : (len + 2 + 6) / 7;
  if (_free && _free() < need) return false;  // all of it or none: a transfer cut short is useless
  ElcCanFrame fr[ELC_CH_MAX_FRAMES];
  uint8_t &tid = _tid[(base >> 5) & 31];
  const size_t n = elc_ch_frames((uint16_t)(base + _node), payload, len, tid, fr, ELC_CH_MAX_FRAMES);
  if (!n) return false;
  tid = (uint8_t)((tid + 1) & 0x1F);
  bool ok = true;
  for (size_t i = 0; i < n; i++) ok = _tx(fr[i]) && ok;
  if (ok) _out++;
  else _send_fail++;
  return ok;
}

void CardNet::announce() {
  uint8_t b[64];
  const size_t n = elc_net_encode_announce(_me, b, sizeof(b));
  if (n) send(ELC_CH_NET, b, n);
}

void CardNet::descriptionsChanged() {
  _me.generation = (uint8_t)(_me.generation % 255 + 1);
  if (_booted) {
    announce();
    if (_h.describe) _h.describe(ELC_NET_Q_ALL & ~ELC_NET_Q_ANNOUNCE);
  }
}

void CardNet::setFactoryDefaults(bool on) {
  const uint8_t f = (uint8_t)((_me.flags & ~1) | (on ? 1 : 0));
  if (f == _me.flags) return;
  _me.flags = f;
  if (_booted) announce();
}

void CardNet::feed(const ElcCanFrame &f, uint32_t now) {
  const uint16_t base = elc_ch_base(f.id);
  if (!base || elc_ch_src(f.id) == _node) return;
  if (base != ELC_CH_NET && base != ELC_CH_UI && base != ELC_CH_ALERT && base != ELC_CH_SYSTEM) return;
  ElcChTransfer t;
  if (!_rx.feed(f, now, t)) return;
  _in++;
  if (base == ELC_CH_NET) {
    const uint8_t op = t.op();
    if (t.for_me(_node) && t.len >= 3) {
      if (op == ELC_NET_QUERY) {  // answer after (node x 8) ms so everyone does not talk at once
        if (!_pending_what) _pending_at = now + (uint32_t)_node * 8u;
        _pending_what |= t.data[2];
      } else if ((op == ELC_NET_IDENTIFY || op == ELC_NET_RESTART) && _h.net_cmd) {
        _h.net_cmd(op, t.data, t.len);
      }
    }
    return;
  }
  if (_h.transfer) _h.transfer(t);
}

void CardNet::poll(uint32_t now) {
  if (!_tx) return;
  if (!_booted) {
    _booted = true;
    announce();
    if (_h.describe) _h.describe(ELC_NET_Q_ALL & ~ELC_NET_Q_ANNOUNCE);
    return;
  }
  if (_pending_what && (int32_t)(now - _pending_at) >= 0) {
    const uint8_t what = _pending_what;
    _pending_what = 0;
    if (what & ELC_NET_Q_ANNOUNCE) announce();
    if ((what & ~ELC_NET_Q_ANNOUNCE) && _h.describe) _h.describe(what & ~ELC_NET_Q_ANNOUNCE);
  }
}
