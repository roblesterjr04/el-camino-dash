#pragma once

// Smaller copies of the phone's cover art for the CAN bus (docs/protocols/can_bus.md, album art):
//   64×64 JPEG — the center shows this first, then steps up to the original;
//   48×48 grey, 4 bits per pixel (1152 bytes, row-major, left pixel in the high nibble) — the console OLED.
// Needs the JPEGDEC + JPEGENC libraries (PlatformIO build); without them art_make_variants() returns 0.

#include <stddef.h>
#include <stdint.h>

// With PSRAM (BOARD_HAS_PSRAM: ESP32-DevKitC-VE / WROVER-E, the carrier's WROVER-E-N8R8) the work buffers live
// there and the small copy is 128 px. Without it (the WROOM DevKitC-32) they must fit in internal RAM, which has only
// ~32 KB free while a phone streams, so the copy is 64 px (~37 KB of work) and is usually skipped.
#ifdef BOARD_HAS_PSRAM
static const int kArtSmallSide = 128;
#else
static const int kArtSmallSide = 64;
#endif
static const int kArtGraySide = 48;
static const size_t kArtGrayBytes = (size_t)kArtGraySide * kArtGraySide / 2;

enum : uint8_t { kArtHaveJpegSmall = 1, kArtHaveGray48 = 2 };

/** Internal heap left for the A2DP stream while the variants are made (no PSRAM): below this the stream stalls. */
static const size_t kArtHeapReserve = 24 * 1024;

/** Large art buffers: PSRAM when the board has it, else internal RAM. nullptr on failure; free with free(). */
void *art_alloc(size_t n);

/** Peak heap art_make_variants() allocates (decoder, encoder, small frame, one strip of a 200 px cover). */
size_t art_variants_work_bytes();

/**
 * Decode `jpg` (centre-cropped to a square), resample to kArtSmallSide² and re-encode into out_jpeg (up to out_cap
 * bytes), and box-average that into the 48×48 grey image. Returns kArtHave* bits for what was made. Uses
 * art_variants_work_bytes() of heap while it runs (PSRAM when present) and frees it all before returning. Without
 * PSRAM, if that would leave the Bluetooth stack less than kArtHeapReserve, it makes nothing and returns 0. It never
 * aborts on a failed allocation.
 */
uint8_t art_make_variants(const uint8_t *jpg, size_t len, uint8_t *out_jpeg, size_t out_cap, size_t *out_len,
                          uint8_t gray48[kArtGrayBytes]);
