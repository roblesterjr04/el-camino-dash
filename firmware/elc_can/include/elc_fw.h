#pragma once

// Firmware update over CAN (docs/protocols/can_channels.md, BULK, The firmware profile).
//
// An update is a BULK WRITE of kind 2 whose object is an .elcfw file: a 64-byte header (this file) and then the
// image exactly as PlatformIO builds it (firmware.bin). The header says which product and which build
// (PlatformIO environment) the image is for, so a screen built for the 4" panel never takes the 3.4" build. The
// post-build script firmware/scripts/elc_fw.py writes the file next to firmware.bin.
//
// ElcFwTarget is the receiving end, one per board: added to the board's ElcChannels as a service, it answers
// WRITEs of its own product and build, checks the header before anything touches flash, writes the image through an
// ElcFwFlash (the board's spare OTA slot: elc_fw_esp32.h) and checks the CRC. The board restarts into the new image
// once it says END ok. No Arduino or ESP-IDF dependencies: host-tested (test/host/test_elc_bulk.cpp).

#include <stddef.h>
#include <stdint.h>

#include "elc_bulk.h"

static const uint8_t ELC_FW_MAGIC[4] = {'E', 'L', 'F', 'W'};
static const size_t ELC_FW_HEADER_SIZE = 64;
static const uint8_t ELC_FW_HEADER_VERSION = 1;

/** The .elcfw header (64 bytes, little-endian):
 *   0  magic "ELFW"          4  header version u8 (1)   5  header size u8 (64)
 *   6  product u16           8  firmware version u16    10 flags u16 (0)
 *   12 image size u32        16 image CRC-32 u32        20 build time u32 (UTC s)
 *   24 variant (32 bytes, NUL-padded: the PlatformIO environment)
 *   56 commit u32 (first 8 hex digits of the git commit, 0 = unknown)
 *   60 CRC-32 of bytes 0..59 */
struct ElcFwHeader {
  uint16_t product = 0;
  uint16_t fw = 0;
  uint16_t flags = 0;
  uint32_t image_size = 0;
  uint32_t image_crc = 0;
  uint32_t build_time = 0;
  uint32_t commit = 0;
  char variant[33] = {0};
};
size_t elc_fw_header_encode(const ElcFwHeader &h, uint8_t out[ELC_FW_HEADER_SIZE]);
/** False for a bad magic, version, size or header CRC. */
bool elc_fw_header_decode(const uint8_t *p, size_t n, ElcFwHeader &out);
/** The WRITE that offers this file to every node of its product and build (dest 0) or to one node. */
ElcBulkWrite elc_fw_offer(const ElcFwHeader &h, uint8_t dest = 0);

/** Where a target puts the image: the board's spare OTA slot (elc_fw_esp32.h), or memory in the host tests. */
class ElcFwFlash {
 public:
  virtual ~ElcFwFlash() {}
  /** The largest image it can take. */
  virtual uint32_t capacity() = 0;
  virtual bool begin(uint32_t image_size) = 0;
  /** Image bytes, in order. */
  virtual bool write(const uint8_t *p, size_t n) = 0;
  /** Every byte is in and the CRC matched: check the image and make it the one that boots next. Returns
   *  ELC_BULK_OK, ELC_BULK_REJECTED (not a valid image), or ELC_BULK_PENDING while it works on (then committing()). */
  virtual uint8_t commit() = 0;
  virtual uint8_t committing() { return ELC_BULK_OK; }
  virtual void abort() = 0;
  /** A short reason for the last failure, for consoles. */
  virtual const char *error() const { return ""; }
};

class ElcFwTarget : public ElcChService, private ElcBulkReceiver::Sink {
 public:
  enum class Phase : uint8_t { Idle, Receiving, Writing, Done, Failed };
  static const uint32_t kRestartMs = 1500;  // after END ok, so the END repeats get out

  /** variant: this build's PlatformIO environment (ELC_FW_VARIANT). */
  void begin(ElcChannels *ch, ElcFwFlash *flash, uint16_t product, const char *variant, uint16_t fw);
  /** Asked when a WRITE arrives: false refuses it as "not now" (the vehicle is moving). Default: always. */
  void setAllowed(bool (*fn)(void *ctx), void *ctx) {
    _allowed = fn;
    _allowed_ctx = ctx;
  }

  void chFrame(const ElcCanFrame &f, uint32_t now_ms) override { _rx.frame(f, now_ms); }
  void chTransfer(const ElcChTransfer &t, uint32_t now_ms) override { _rx.transfer(t, now_ms); }
  void chPoll(uint32_t now_ms) override;

  Phase phase() const { return _phase; }
  bool active() const { return _phase == Phase::Receiving || _phase == Phase::Writing; }
  uint16_t progress() const { return _rx.progress(); }
  uint8_t writer() const { return _rx.writer(); }
  uint8_t result() const { return _rx.result(); }
  const ElcFwHeader &header() const { return _hdr; }
  /** True once, kRestartMs after an update finished well: the board restarts into the new image. */
  bool restartDue(uint32_t now_ms);
  /** One line for bench consoles. */
  void describe(char *out, size_t n) const;

 private:
  uint8_t accept(const ElcBulkWrite &w, uint8_t from) override;
  uint8_t write(uint32_t offset, const uint8_t *p, size_t n) override;
  uint32_t crcFrom() const override { return ELC_FW_HEADER_SIZE; }
  uint8_t finish(bool crc_ok) override;
  uint8_t finishing() override;
  void aborted(uint8_t result) override;

  ElcBulkReceiver _rx;
  ElcFwFlash *_flash = nullptr;
  uint16_t _product = 0, _fw = 0;
  char _variant[33] = {0};
  bool (*_allowed)(void *) = nullptr;
  void *_allowed_ctx = nullptr;
  Phase _phase = Phase::Idle;
  ElcBulkWrite _w;
  uint8_t _head[ELC_FW_HEADER_SIZE];
  ElcFwHeader _hdr;
  bool _head_ok = false;
  bool _flash_open = false;
  uint32_t _done_ms = 0;
  bool _restart_said = false;
  const char *_why = "";
};
