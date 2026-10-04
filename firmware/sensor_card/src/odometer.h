#pragma once

// The odometer and the card's FRAM blocks (docs/protocols/sensor_uart.md, Odometer storage; sensor_calibration.md,
// Where it is stored). The slot format is sensor_pack's v3, unchanged, so a mileage already in a FRAM survives the
// move to the card. No Arduino dependencies: host-tested.

#include <stddef.h>
#include <stdint.h>

#include "calib.h"

/** The FRAM as the logic sees it (fram.cpp is the I2C one, the tests use an array). */
struct FramIo {
  bool (*probe)(void *ctx);
  bool (*read)(void *ctx, uint16_t addr, uint8_t *buf, uint8_t len);
  bool (*write)(void *ctx, uint16_t addr, const uint8_t *buf, uint8_t len);
  void *ctx;
};

// Layout (bytes)
static const uint16_t kFramOdoLegacy = 0x0000;  // 12, read once for migration, never written
static const uint16_t kFramOdoA = 0x0010;       // 20 each
static const uint16_t kFramOdoB = 0x0030;
static const uint16_t kFramIdentity = 0x0050;   // 8: C1 D0, uid u32, reserved, CRC-8
static const uint16_t kFramCalA = 0x0100;       // 512 each
static const uint16_t kFramCalB = 0x0300;

static const uint32_t kOdoMaxTenths = 99999990UL;
static const uint32_t kOdoUnknown = 0xFFFFFFFFUL;

enum class OdoLoad : uint8_t { Ok = 0, NoChip = 1, Corrupt = 2 };
/** Why a stamp ran (decides which status line is printed). */
enum class OdoStamp : uint8_t { Silent, Frac, Tenth, Init };

class Odometer {
 public:
  void begin(const FramIo &io, uint32_t seed_tenths) {
    _io = io;
    _seed = seed_tenths;
  }
  /** One load attempt: the newest valid slot, else the legacy blob (migrated), else the seed, but only into a chip that
   *  answers and reads fully blank. Never writes in any other case. */
  OdoLoad tryLoad();
  bool known() const { return _known; }
  /** What the last load or stamp wants said (a short line, empty if nothing), cleared by takeNote(). */
  const char *note() const { return _note; }
  void clearNote() { _note[0] = 0; }

  uint32_t tenths() const { return _tenths; }
  uint32_t fracNano() const { return _frac_nano; }
  /** Sets the mileage (miles x10): a replacement card, or a bench card with no FRAM (then it only lives in RAM). */
  void set(uint32_t tenths);
  /** Adds distance in nano-miles; a crossed tenth stamps at once. Returns the tenths crossed. */
  uint32_t add(uint32_t nano_miles);
  /** Sub-tenth progress, written every couple of seconds while moving. */
  void stampFrac() { stamp(OdoStamp::Frac); }
  /** Last stamp reached the chip. A card with no chip keeps counting in RAM and says so. */
  bool persisted() const { return _persisted; }

 private:
  void stamp(OdoStamp why);
  void adopt(uint32_t tenths, uint32_t frac_nano);
  void say(const char *s);
  FramIo _io = {nullptr, nullptr, nullptr, nullptr};
  uint32_t _seed = 0;
  uint32_t _tenths = 0, _frac_nano = 0;
  bool _known = false, _persisted = false;
  uint8_t _slot = 1;  // last written / loaded (0 A, 1 B)
  uint32_t _seq = 0;
  char _note[20] = {0};
};

uint8_t odo_crc8(const uint8_t *p, size_t n);

// ---- calibration copies (alternating A / B, newest valid sequence wins)

class CalStore {
 public:
  void begin(const FramIo &io) { _io = io; }
  /** Loads the newest valid copy into c. False when neither copy is valid (a new card or an unreadable chip). */
  bool load(cal::Calibration &c);
  /** Writes the next copy. False if the chip did not take it. */
  bool save(const cal::Calibration &c);
  uint32_t seq() const { return _seq; }

 private:
  FramIo _io = {nullptr, nullptr, nullptr, nullptr};
  uint8_t _slot = 1;
  uint32_t _seq = 0;
};

/** The card's unique ID block: false when the chip holds none (blank). */
bool identity_read(const FramIo &io, uint32_t *uid);
bool identity_write(const FramIo &io, uint32_t uid);
