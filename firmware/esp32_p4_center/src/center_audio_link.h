#pragma once

#include <Arduino_GFX_Library.h>
#include <stdint.h>

// Reads AVRCP "now playing" text from esp32_audio_bridge.ino (I2C slave 0x2A, register 0x01),
// same contract as integrations.esp32_audio_link on the Pi. Uses the already-initialized Wire bus
// (Waveshare: SDA=7, SCL=8).

void center_audio_link_begin();
void center_audio_link_poll();
bool center_audio_link_ok();
const char *center_audio_link_line();

// One-line ticker at bottom of gauge (call after center_render; skipped when empty / offline).
void center_audio_link_draw(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h);
