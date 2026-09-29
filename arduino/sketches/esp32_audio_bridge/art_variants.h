#pragma once

// Smaller copies of the phone's cover art for the CAN bus (docs/protocols/can_bus.md, album art):
//   128×128 JPEG — the center shows this first, then steps up to the original;
//   48×48 grey, 4 bits per pixel (1152 bytes, row-major, left pixel in the high nibble) — the console OLED.
// Needs the JPEGDEC + JPEGENC libraries (PlatformIO build); without them art_make_variants() returns 0.

#include <stddef.h>
#include <stdint.h>

static const int kArtSmallSide = 128;
static const int kArtGraySide = 48;
static const size_t kArtGrayBytes = (size_t)kArtGraySide * kArtGraySide / 2;

enum : uint8_t { kArtHaveJpeg128 = 1, kArtHaveGray48 = 2 };

/**
 * Decode `jpg` (centre-cropped to a square), resample to 128×128 and re-encode into out_jpeg (up to out_cap
 * bytes), and box-average that into the 48×48 grey image. Returns kArtHave* bits for what was made. Uses
 * about 45 KB of heap while it runs, and frees it all before returning.
 */
uint8_t art_make_variants(const uint8_t *jpg, size_t len, uint8_t *out_jpeg, size_t out_cap, size_t *out_len,
                          uint8_t gray48[kArtGrayBytes]);
