#include "elc_store.h"

#include <stdio.h>
#include <string.h>

void ElcStoreServer::begin(ElcChannels *ch, ElcStoreBackend *backend) {
  _ch = ch;
  _b = backend;
}

bool ElcStoreServer::owner(uint8_t node, char *out, size_t cap) const {
  if (cap) out[0] = 0;
  if (!_ch || !_ch->nodeInfo(node).announced) return false;
  snprintf(out, cap, "%04X_%02X", (unsigned)_ch->nodeInfo(node).a.product, (unsigned)(node & 0x1F));
  return true;
}

bool ElcStoreServer::handle(const ElcChTransfer &t) {
  if (!_ch || !_b || t.base != ELC_CH_STORE || !t.for_me(_ch->node())) return false;
  const uint8_t op = t.op();
  if (op != ELC_STORE_PUT && op != ELC_STORE_GET && op != ELC_STORE_DELETE) return false;
  if (op == ELC_STORE_GET) {
    _stats.gets++;
    get(t);
    return true;
  }
  uint8_t tag = t.len >= 3 ? t.data[2] : 0;
  uint32_t size = 0;
  uint8_t result;
  if (op == ELC_STORE_PUT) {
    _stats.puts++;
    result = put(t, &tag, &size);
  } else {
    _stats.deletes++;
    result = del(t, &tag);
  }
  reply(t.src, tag, result, size);
  _stats.last_node = t.src;
  _stats.last_op = op;
  _stats.last_result = result;
  return true;
}

uint8_t ElcStoreServer::check(uint8_t src, const char *key, char *own) {
  if (!elc_store_key_ok(key)) return ELC_STORE_BAD_KEY;
  if (!owner(src, own, kOwnerCap)) {
    _ch->query(src, ELC_NET_Q_ANNOUNCE);  // the owner name needs its product: ask, and the sender retries
    return ELC_STORE_BUSY;
  }
  return _b->ready() ? ELC_STORE_OK : ELC_STORE_NO_MEDIA;
}

uint8_t ElcStoreServer::put(const ElcChTransfer &t, uint8_t *tag, uint32_t *size) {
  ElcStorePut v;
  if (!elc_store_decode(t, v) || v.mode > ELC_STORE_APPEND) return ELC_STORE_BAD;
  *tag = v.tag;
  char own[kOwnerCap];
  const uint8_t c = check(t.src, v.key, own);
  if (c != ELC_STORE_OK) return c;
  const int32_t cur = _b->size(own, v.key);
  *size = cur > 0 ? (uint32_t)cur : 0;
  const uint32_t after = v.mode == ELC_STORE_APPEND ? *size + (uint32_t)v.len : (uint32_t)v.len;
  if (after > ELC_STORE_RECORD_MAX) return ELC_STORE_FULL;
  if (cur < 0 && _b->count(own) >= ELC_STORE_RECORDS_MAX) return ELC_STORE_FULL;
  if (!_b->write(own, v.key, v.data, v.len, v.mode == ELC_STORE_APPEND)) {
    const int32_t now = _b->size(own, v.key);
    *size = now > 0 ? (uint32_t)now : 0;
    return ELC_STORE_IO;
  }
  *size = after;
  _stats.bytes_in += (uint32_t)v.len;
  return ELC_STORE_OK;
}

uint8_t ElcStoreServer::del(const ElcChTransfer &t, uint8_t *tag) {
  ElcStoreDelete v;
  if (!elc_store_decode(t, v)) return ELC_STORE_BAD;
  *tag = v.tag;
  char own[kOwnerCap];
  const uint8_t c = check(t.src, v.key, own);
  if (c != ELC_STORE_OK) return c;
  if (_b->size(own, v.key) < 0) return ELC_STORE_NOT_FOUND;
  return _b->remove(own, v.key) ? ELC_STORE_OK : ELC_STORE_IO;
}

uint8_t ElcStoreServer::read(uint8_t src, const ElcStoreGet &v, ElcStoreData *d, uint8_t *buf) {
  d->tag = v.tag;
  d->offset = v.offset;
  char own[kOwnerCap];
  const uint8_t c = check(src, v.key, own);
  if (c != ELC_STORE_OK) return c;
  const int32_t cur = _b->size(own, v.key);
  if (cur < 0) return ELC_STORE_NOT_FOUND;
  d->size = (uint32_t)cur;
  if (v.offset > d->size) return ELC_STORE_BAD;
  size_t want = v.max && v.max < ELC_STORE_DATA_MAX ? v.max : ELC_STORE_DATA_MAX;
  if (want > d->size - v.offset) want = d->size - v.offset;
  const int32_t n = want ? _b->read(own, v.key, v.offset, buf, want) : 0;
  if (n < 0) return ELC_STORE_IO;
  d->data = buf;
  d->len = (size_t)n;
  _stats.bytes_out += (uint32_t)n;
  return ELC_STORE_OK;
}

void ElcStoreServer::get(const ElcChTransfer &t) {
  ElcStoreGet v;
  ElcStoreData d;
  d.dest = t.src;
  d.tag = t.len >= 3 ? t.data[2] : 0;
  uint8_t buf[ELC_STORE_DATA_MAX];
  d.result = elc_store_decode(t, v) ? read(t.src, v, &d, buf) : ELC_STORE_BAD;
  if (d.result != ELC_STORE_OK) _stats.refused++;
  _stats.last_node = t.src;
  _stats.last_op = ELC_STORE_GET;
  _stats.last_result = d.result;
  ElcChWriter w;
  elc_store_encode(d, w);
  _ch->send(ELC_CH_STORE, w);
}

void ElcStoreServer::reply(uint8_t to, uint8_t tag, uint8_t result, uint32_t size) {
  if (result != ELC_STORE_OK) _stats.refused++;
  ElcStoreResult r;
  r.dest = to;
  r.tag = tag;
  r.result = result;
  r.size = size;
  ElcChWriter w;
  elc_store_encode(r, w);
  _ch->send(ELC_CH_STORE, w);
}
