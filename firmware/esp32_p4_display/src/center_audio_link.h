#pragma once

#include <Arduino_GFX_Library.h>
#include <stddef.h>
#include <stdint.h>

#include <elc_can_codec.h>

// Now playing, transport and album art from esp32_audio_bridge.ino. In the car this all comes over CAN
// (docs/protocols/can_bus.md); on the bench, through the USB CAN link (docs/protocols/usb_link.md).

void center_audio_link_begin();
void center_audio_link_poll();
/** Fetch album art over CAN (the center is the media screen). Frames are fed to media_can by center_can. */
void center_audio_link_set_fetch_art(bool on);
bool center_audio_link_ok();

/** One line describing the album-art transfer, for the `audio` console command. */
void center_audio_art_status(char *out, size_t n);
/** Album art of the current track, decoded to RGB565 (row length = stride pixels). serial changes per
 *  image. False when the track has no art or it has not arrived yet. */
bool center_audio_art(const uint16_t **px, int *w, int *h, int *stride, uint32_t *serial);
const char *center_audio_link_line();

// Playback transport from the bridge (CAN): elapsed/duration seconds, volume 0–15. Returns false until first valid read.
bool center_audio_link_transport(uint16_t *elapsed_s, uint16_t *duration_s, uint8_t *vol_0_15);

