#pragma once

#include <Arduino_GFX_Library.h>
#include <stddef.h>
#include <stdint.h>

// Reads AVRCP "now playing" text from esp32_audio_bridge.ino (I2C slave 0x2A, register 0x01),
// same contract as integrations.esp32_audio_link on the Pi. Uses the already-initialized Wire bus
// (Waveshare: SDA=7, SCL=8).

void center_audio_link_begin();
void center_audio_link_poll();
bool center_audio_link_ok();

/** Album art of the current track, decoded to RGB565 (row length = stride pixels). serial changes per
 *  image. False when the track has no art or it has not arrived yet. */
/** One line describing the album-art transfer, for the `audio` console command. */
void center_audio_art_status(char *out, size_t n);
bool center_audio_art(const uint16_t **px, int *w, int *h, int *stride, uint32_t *serial);
/** Bytes received by the last snapshot/transport read (-1 = no ACK), for the `audio` console command. */
int center_audio_link_last_rx();
const char *center_audio_link_line();

// Playback transport from Pi (I2C reg 0x05): elapsed/duration seconds, volume 0–15. Returns false until first valid read.
bool center_audio_link_transport(uint16_t *elapsed_s, uint16_t *duration_s, uint8_t *vol_0_15);

