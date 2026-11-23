#pragma once

// Firmware update over CAN on an ESP32 board (ESP32, -S3, -P4): the ElcFwTarget of elc_fw.h writing into the spare OTA
// slot, and the rollback that makes a bad image harmless when the board's USB port cannot be reached.
//
//   setup():  elc_fw_esp32_begin(&channels, ELC_PRODUCT_…, version) once the channels are attached
//   loop():   elc_fw_esp32_poll(millis(), healthy)  (healthy: the board is doing its job, e.g. CAN is up)
//
// The image is staged in PSRAM when the board has room (the P4 screens): nothing touches flash until every byte has
// arrived and the CRC matched, and the bus is never kept waiting by a flash erase. Without PSRAM (the console) it is
// written as it arrives. Either way the Arduino Update library writes the slot and checks the image (its SHA-256)
// before it is made the boot image.
//
// Rollback: Arduino-ESP32 3.x bootloaders have app rollback on. A new image boots "pending verify"; this file asks the
// core not to accept it at boot (verifyRollbackLater), and elc_fw_esp32_poll() accepts it once the board has run
// healthy for ELC_FW_GOOD_MS. Any reset before that (a crash, the watchdog, the ignition turned off) boots the old
// image again. Set -D ELC_FW_ROLLBACK=0 to accept every image at boot instead.
//
// The build is identified by ELC_FW_VARIANT (the PlatformIO environment, set by firmware/scripts/elc_fw.py): a WRITE
// for another build of the same product is refused. A build without that script gets do-nothing stubs (and no change to
// how its images boot).

#include <stddef.h>
#include <stdint.h>

#include "elc_fw.h"

#ifndef ELC_FW_VARIANT
#define ELC_FW_VARIANT "unknown"
#endif
#ifndef ELC_FW_GOOD_MS
#define ELC_FW_GOOD_MS 20000
#endif
#ifndef ELC_FW_ROLLBACK
#define ELC_FW_ROLLBACK 1
#endif

/** Starts the target on these channels. Prints what booted (and whether the last update was rolled back). */
void elc_fw_esp32_begin(ElcChannels *ch, uint16_t product, uint16_t fw_version);
/** Restarts into a new image when one finished; accepts the running image after ELC_FW_GOOD_MS of healthy polls. */
void elc_fw_esp32_poll(uint32_t now_ms, bool healthy);
/** Only while vehicle_stopped() says yes are updates taken (the screens show the progress instead of the gauges). */
void elc_fw_esp32_set_allowed(bool (*vehicle_stopped)(void *ctx), void *ctx);
ElcFwTarget *elc_fw_esp32_target();
/** The running image is still on trial (it rolls back if the board resets now). */
bool elc_fw_esp32_pending();
/** The serial console's `fw` command: state, partitions, `fw good` (accept the running image now). True if handled. */
bool elc_fw_esp32_command(const char *line, char *reply, size_t n);

/** The spare OTA slot as an ElcFwFlash; also used directly by the console's Wi-Fi upload. */
class ElcFwFlashEsp32 : public ElcFwFlash {
 public:
  uint32_t capacity() override;
  bool begin(uint32_t image_size) override;
  bool write(const uint8_t *p, size_t n) override;
  uint8_t commit() override;
  uint8_t committing() override;
  void abort() override;
  const char *error() const override { return _err; }
  /** Stage in PSRAM when there is room (default), or always write as it arrives. */
  void allowStaging(bool on) { _allow_staging = on; }

  void run_commit();  // the commit task's body (internal)

 private:
  uint8_t *_stage = nullptr;  // PSRAM copy of the image, or null when writing straight to flash
  uint32_t _size = 0, _have = 0;
  bool _open = false, _allow_staging = true;
  volatile uint8_t _commit_result = ELC_BULK_OK;
  volatile bool _committing = false;
  char _err[48] = {0};
};
