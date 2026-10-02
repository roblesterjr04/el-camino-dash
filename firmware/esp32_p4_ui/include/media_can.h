#pragma once

#include <stddef.h>
#include <stdint.h>

#include <elc_can_codec.h>
#include <elc_gauges.h>

/**
 * Now playing and album art from the audio bridge on CAN (docs/protocols/can_bus.md), for whichever round
 * display is the media screen. Every display hears the bridge's transport and text; only the one that shows
 * media fetches album art: the 128×128 copy first, then the original. The P4's hardware JPEG decoder turns it
 * into RGB565 for the media face.
 *
 * Loop contract: pass every CAN frame to media_can_frame(), call media_can_poll() once per loop.
 */

/** send: how this board puts a frame on the bus (used for art requests). */
void media_can_begin(bool (*send)(const ElcCanFrame &f));

/** Feed one received frame. True when it was the bridge's (transport, text or art). */
bool media_can_frame(const ElcCanFrame &f);

/** Art pump. fetch_art = this display shows media now; otherwise no art is requested. */
void media_can_poll(bool fetch_art);

/** A bridge was heard within max_age_ms. line gets "Artist - Title" ("" with no phone or nothing playing). */
bool media_can_now_playing(char *line, size_t n, uint16_t *elapsed_s, uint16_t *duration_s, uint8_t *vol_0_15,
                           uint32_t max_age_ms = 2000);

/** Album art of the current track (row length = stride pixels). serial changes per image. */
bool media_art(const uint16_t **px, int *w, int *h, int *stride, uint32_t *serial);

/** Download buffer the decoder can read (DMA-capable). */
uint8_t *media_art_buffer(size_t *cap);
/** Decode a JPEG into the current album art. */
bool media_art_decode(const uint8_t *jpg, size_t len);
/** Drop the current album art (new track, bridge gone). */
void media_art_clear();

/** One line describing the CAN art transfer, for the `audio` console command. */
void media_can_art_status(char *out, size_t n);

/** Fill the media fields of GaugeData from the CAN bridge (position interpolated between transport frames). */
void media_can_fill(GaugeData &d, uint32_t now_ms);
