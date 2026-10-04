#include "odometer.h"

#include <string.h>

static const uint8_t kHdr0 = 0xE1, kHdr1 = 0xD0, kVerSlot = 0x03, kVerLegacy = 0x01, kVerFrac = 0x02;
static const uint8_t kSlotLen = 20;

uint8_t odo_crc8(const uint8_t *p, size_t n) {
  uint8_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (uint8_t b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  }
  return crc;
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool decode_slot(const uint8_t *b, uint32_t *seq, uint32_t *tenths, uint32_t *frac) {
  if (b[0] != kHdr0 || b[1] != kHdr1 || b[2] != kVerSlot) return false;
  if (odo_crc8(b, kSlotLen - 1) != b[kSlotLen - 1]) return false;
  *seq = get_be32(b + 4);
  *tenths = get_be32(b + 8);
  *frac = get_be32(b + 12);
  return *tenths <= kOdoMaxTenths && *frac <= 99999999UL;
}

void Odometer::say(const char *s) {
  strncpy(_note, s, sizeof(_note) - 1);
  _note[sizeof(_note) - 1] = 0;
}

void Odometer::adopt(uint32_t tenths, uint32_t frac_nano) {
  // distance driven while the stored value was still unknown is kept and added on top
  const uint32_t pending_t = _tenths;
  uint32_t fn = _frac_nano + frac_nano;
  uint32_t t = tenths + pending_t;
  while (fn >= 100000000UL && t < kOdoMaxTenths) {
    fn -= 100000000UL;
    t++;
  }
  _tenths = t;
  _frac_nano = fn;
  _known = true;
}

void Odometer::stamp(OdoStamp why) {
  if (!_known) return;  // never write a placeholder over the real mileage
  if (!_io.write) {
    _persisted = false;
    return;
  }
  uint8_t b[kSlotLen];
  memset(b, 0, sizeof(b));
  b[0] = kHdr0;
  b[1] = kHdr1;
  b[2] = kVerSlot;
  const uint32_t seq = _seq + 1;
  put_be32(b + 4, seq);
  put_be32(b + 8, _tenths);
  put_be32(b + 12, _frac_nano > 99999999UL ? 99999999UL : _frac_nano);
  b[kSlotLen - 1] = odo_crc8(b, kSlotLen - 1);
  const uint8_t slot = (uint8_t)(_slot ^ 1);
  bool ok = false;
  for (uint8_t attempt = 0; attempt < 3 && !ok; attempt++) {
    ok = _io.write(_io.ctx, slot ? kFramOdoB : kFramOdoA, b, kSlotLen);
  }
  if (!ok) {
    _persisted = false;
    say("FRAM wr ERR");
    return;
  }
  _seq = seq;
  _slot = slot;
  _persisted = true;
  if (why == OdoStamp::Tenth) say("FRAM wr 10th");
  else if (why == OdoStamp::Init) say("FRAM wr init");
}

OdoLoad Odometer::tryLoad() {
  if (!_io.probe || !_io.probe(_io.ctx)) return OdoLoad::NoChip;
  uint8_t a[kSlotLen], b[kSlotLen];
  if (!_io.read(_io.ctx, kFramOdoA, a, kSlotLen) || !_io.read(_io.ctx, kFramOdoB, b, kSlotLen)) return OdoLoad::NoChip;
  uint32_t sa = 0, ta = 0, fa = 0, sb = 0, tb = 0, fb = 0;
  const bool ok_a = decode_slot(a, &sa, &ta, &fa);
  const bool ok_b = decode_slot(b, &sb, &tb, &fb);
  if (ok_a || ok_b) {
    const bool use_b = ok_b && (!ok_a || (int32_t)(sb - sa) > 0);  // serial-number compare: the sequence may wrap
    _slot = use_b ? 1 : 0;
    _seq = use_b ? sb : sa;
    adopt(use_b ? tb : ta, use_b ? fb : fa);
    _persisted = true;
    say(ok_a && ok_b ? "FRAM rd v3 OK" : "FRAM rd v3 1slot");
    return OdoLoad::Ok;
  }
  uint8_t l[12];
  if (!_io.read(_io.ctx, kFramOdoLegacy, l, sizeof(l))) return OdoLoad::NoChip;
  if (l[0] == kHdr0 && l[1] == kHdr1 && (l[2] == kVerFrac || l[2] == kVerLegacy)) {
    const uint32_t tenths = get_be32(l + 4);
    uint32_t fn = l[2] == kVerFrac ? get_be32(l + 8) : 0;
    if (tenths <= kOdoMaxTenths) {
      if (fn > 99999999UL) fn = 0;
      _slot = 1;  // the first stamp goes to slot A
      _seq = 0;
      adopt(tenths, fn);
      say("FRAM rd legacy");
      stamp(OdoStamp::Init);
      return OdoLoad::Ok;
    }
  }
  // seed only a genuinely blank chip: legacy blob and both slots all 0x00 or all 0xFF
  bool all00 = true, allff = true;
  const uint8_t *regions[3] = {l, a, b};
  const uint8_t lens[3] = {sizeof(l), kSlotLen, kSlotLen};
  for (int r = 0; r < 3; r++) {
    for (uint8_t i = 0; i < lens[r]; i++) {
      all00 = all00 && regions[r][i] == 0x00;
      allff = allff && regions[r][i] == 0xFF;
    }
  }
  if (!all00 && !allff) return OdoLoad::Corrupt;
  _slot = 1;
  _seq = 0;
  adopt(_seed, 0);
  say("FRAM blank");
  stamp(OdoStamp::Init);
  return OdoLoad::Ok;
}

void Odometer::set(uint32_t tenths) {
  if (tenths > kOdoMaxTenths) tenths = kOdoMaxTenths;
  _tenths = tenths;
  _frac_nano = 0;
  _known = true;
  stamp(OdoStamp::Init);
}

uint32_t Odometer::add(uint32_t nano) {
  uint32_t crossed = 0;
  _frac_nano += nano;
  while (_frac_nano >= 100000000UL && _tenths < kOdoMaxTenths) {
    _frac_nano -= 100000000UL;
    _tenths++;
    crossed++;
    stamp(OdoStamp::Tenth);
  }
  return crossed;
}

// ---- calibration copies

bool CalStore::load(cal::Calibration &c) {
  if (!_io.read) return false;
  static uint8_t buf[cal::kBlockMax];  // one at a time; the caller is single-threaded
  cal::Calibration ca, cb;
  uint32_t sa = 0, sb = 0;
  bool ok_a = false, ok_b = false;
  if (_io.read(_io.ctx, kFramCalA, buf, sizeof(buf))) ok_a = cal::block_decode(buf, sizeof(buf), &sa, ca);
  if (_io.read(_io.ctx, kFramCalB, buf, sizeof(buf))) ok_b = cal::block_decode(buf, sizeof(buf), &sb, cb);
  if (!ok_a && !ok_b) return false;
  const bool use_b = ok_b && (!ok_a || (int32_t)(sb - sa) > 0);
  _slot = use_b ? 1 : 0;
  _seq = use_b ? sb : sa;
  c = use_b ? cb : ca;
  return true;
}

bool CalStore::save(const cal::Calibration &c) {
  if (!_io.write) return false;
  static uint8_t buf[cal::kBlockMax];
  const uint32_t seq = _seq + 1;
  const size_t n = cal::block_encode(c, seq, buf, sizeof(buf));
  if (!n) return false;
  const uint8_t slot = (uint8_t)(_slot ^ 1);
  if (!_io.write(_io.ctx, slot ? kFramCalB : kFramCalA, buf, (uint8_t)n)) return false;
  _slot = slot;
  _seq = seq;
  return true;
}

// ---- identity

bool identity_read(const FramIo &io, uint32_t *uid) {
  uint8_t b[8];
  if (!io.read || !io.read(io.ctx, kFramIdentity, b, sizeof(b))) return false;
  if (b[0] != 0xC1 || b[1] != 0xD0 || odo_crc8(b, 7) != b[7]) return false;
  *uid = (uint32_t)b[2] | (uint32_t)b[3] << 8 | (uint32_t)b[4] << 16 | (uint32_t)b[5] << 24;
  return true;
}

bool identity_write(const FramIo &io, uint32_t uid) {
  uint8_t b[8] = {0xC1, 0xD0, (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16), (uint8_t)(uid >> 24), 0, 0};
  b[7] = odo_crc8(b, 7);
  return io.write && io.write(io.ctx, kFramIdentity, b, sizeof(b));
}
