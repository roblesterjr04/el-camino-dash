#include "elc_fw.h"

#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t elc_fw_header_encode(const ElcFwHeader &h, uint8_t out[ELC_FW_HEADER_SIZE]) {
  memset(out, 0, ELC_FW_HEADER_SIZE);
  memcpy(out, ELC_FW_MAGIC, 4);
  out[4] = ELC_FW_HEADER_VERSION;
  out[5] = (uint8_t)ELC_FW_HEADER_SIZE;
  put16(out + 6, h.product);
  put16(out + 8, h.fw);
  put16(out + 10, h.flags);
  put32(out + 12, h.image_size);
  put32(out + 16, h.image_crc);
  put32(out + 20, h.build_time);
  strncpy((char *)out + 24, h.variant, 32);
  put32(out + 56, h.commit);
  put32(out + 60, elc_crc32(out, 60));
  return ELC_FW_HEADER_SIZE;
}

bool elc_fw_header_decode(const uint8_t *p, size_t n, ElcFwHeader &out) {
  if (n < ELC_FW_HEADER_SIZE || memcmp(p, ELC_FW_MAGIC, 4) != 0) return false;
  if (p[4] != ELC_FW_HEADER_VERSION || p[5] != ELC_FW_HEADER_SIZE) return false;
  if (get32(p + 60) != elc_crc32(p, 60)) return false;
  out = ElcFwHeader();
  out.product = get16(p + 6);
  out.fw = get16(p + 8);
  out.flags = get16(p + 10);
  out.image_size = get32(p + 12);
  out.image_crc = get32(p + 16);
  out.build_time = get32(p + 20);
  memcpy(out.variant, p + 24, 32);
  out.variant[32] = '\0';
  out.commit = get32(p + 56);
  return out.image_size > 0;
}

ElcBulkWrite elc_fw_offer(const ElcFwHeader &h, uint8_t dest) {
  ElcBulkWrite w;
  w.dest = dest;
  w.kind = ELC_BULK_KIND_FIRMWARE;
  w.object = h.product;
  w.size = (uint32_t)ELC_FW_HEADER_SIZE + h.image_size;
  w.crc = h.image_crc;
  w.fw = h.fw;
  strncpy(w.variant, h.variant, sizeof(w.variant) - 1);
  return w;
}

// ---- target ----

void ElcFwTarget::begin(ElcChannels *ch, ElcFwFlash *flash, uint16_t product, const char *variant, uint16_t fw) {
  _flash = flash;
  _product = product;
  _fw = fw;
  strncpy(_variant, variant ? variant : "", sizeof(_variant) - 1);
  _rx.begin(ch, this);
  if (ch) ch->addService(this);
}

uint8_t ElcFwTarget::accept(const ElcBulkWrite &w, uint8_t from) {
  (void)from;
  if (w.kind != ELC_BULK_KIND_FIRMWARE || w.object != _product || !_flash) return ELC_BULK_WHY_KIND;
  if (w.variant[0] && strcmp(w.variant, _variant) != 0) return ELC_BULK_WHY_TARGET;
  if (w.size <= ELC_FW_HEADER_SIZE || w.size - ELC_FW_HEADER_SIZE > _flash->capacity()) return ELC_BULK_WHY_SIZE;
  if (_allowed && !_allowed(_allowed_ctx)) return ELC_BULK_WHY_NOT_NOW;
  _w = w;
  _head_ok = false;
  _flash_open = false;
  _hdr = ElcFwHeader();
  _phase = Phase::Receiving;
  _done_ms = 0;
  _restart_said = false;
  _why = "";
  return ELC_BULK_WHY_NONE;
}

uint8_t ElcFwTarget::write(uint32_t offset, const uint8_t *p, size_t n) {
  if (offset < ELC_FW_HEADER_SIZE) {
    const size_t k = ELC_FW_HEADER_SIZE - offset < n ? ELC_FW_HEADER_SIZE - offset : n;
    memcpy(_head + offset, p, k);
    offset += (uint32_t)k;
    p += k;
    n -= k;
    if (offset == ELC_FW_HEADER_SIZE) {
      // the header, before anything touches flash: this product, this build, the size and CRC the WRITE said
      if (!elc_fw_header_decode(_head, sizeof(_head), _hdr)) {
        _why = "bad header";
        return ELC_BULK_REJECTED;
      }
      if (_hdr.product != _product || strcmp(_hdr.variant, _variant) != 0) {
        _why = "another product or build";
        return ELC_BULK_REJECTED;
      }
      if (_hdr.image_size + ELC_FW_HEADER_SIZE != _w.size || _hdr.image_crc != _w.crc) {
        _why = "header does not match the offer";
        return ELC_BULK_REJECTED;
      }
      if (!_flash->begin(_hdr.image_size)) {
        _why = "no OTA slot";
        return ELC_BULK_NO_SPACE;
      }
      _flash_open = true;
      _head_ok = true;
    }
  }
  if (n) {
    if (!_head_ok) return ELC_BULK_REJECTED;
    if (!_flash->write(p, n)) {
      _why = "flash write failed";
      return ELC_BULK_NO_SPACE;
    }
  }
  return ELC_BULK_OK;
}

uint8_t ElcFwTarget::finish(bool crc_ok) {
  if (!crc_ok || !_head_ok) {
    _why = "CRC mismatch";
    if (_flash_open) _flash->abort();
    _flash_open = false;
    _phase = Phase::Failed;
    return ELC_BULK_CRC;
  }
  _phase = Phase::Writing;
  const uint8_t r = _flash->commit();
  if (r == ELC_BULK_PENDING) return r;
  return finishing();
}

uint8_t ElcFwTarget::finishing() {
  const uint8_t r = _flash->committing();
  if (r == ELC_BULK_PENDING) return r;
  _flash_open = false;
  if (r == ELC_BULK_OK) {
    _phase = Phase::Done;
  } else {
    _phase = Phase::Failed;
    _why = _flash->error()[0] ? _flash->error() : "image rejected";
  }
  return r;
}

void ElcFwTarget::aborted(uint8_t result) {
  if (_flash_open) _flash->abort();
  _flash_open = false;
  if (!_why[0]) _why = result == ELC_BULK_ABORTED ? "aborted" : "stopped";
  _phase = Phase::Failed;
}

void ElcFwTarget::chPoll(uint32_t now_ms) {
  _rx.poll(now_ms);
  if (_phase == Phase::Done && !_done_ms) _done_ms = now_ms ? now_ms : 1;
}

bool ElcFwTarget::restartDue(uint32_t now_ms) {
  if (_phase != Phase::Done || !_done_ms || _restart_said || now_ms - _done_ms < kRestartMs) return false;
  _restart_said = true;
  return true;
}

void ElcFwTarget::describe(char *out, size_t n) const {
  static const char *const kPhase[] = {"idle", "receiving", "writing flash", "done, restarting", "failed"};
  const int w = snprintf(out, n, "fw: %s build %s v%u", kPhase[(int)_phase], _variant, (unsigned)_fw);
  if (w < 0 || (size_t)w >= n) return;
  if (_phase == Phase::Receiving || _phase == Phase::Writing) {
    snprintf(out + w, n - (size_t)w, " from 0x%02X, %u.%u%% (%lu of %lu bytes, %lu asks)", (unsigned)_rx.writer(),
             (unsigned)(progress() / 10), (unsigned)(progress() % 10), (unsigned long)_rx.received(),
             (unsigned long)_w.size, (unsigned long)_rx.asks());
  } else if (_phase == Phase::Failed) {
    snprintf(out + w, n - (size_t)w, ": %s (END %u)", _why, (unsigned)_rx.result());
  }
}
