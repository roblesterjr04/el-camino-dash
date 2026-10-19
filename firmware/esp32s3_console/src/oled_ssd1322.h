#pragma once

// SSD1322 256×64 on 4-wire SPI (the console's OLED module): NHD init table, then whole 4-bit frames. The picture is
// turned 180° here (the module sits upside down in the console), and grey levels are scaled for the contrast setting.

#include <stdint.h>

class OledGfx;

void oled_begin();
/** Send the frame. scale 0..1 dims every grey level (0 = dark, 1 = as drawn); drive_pct is the segment current,
 *  1..100 % of the chip's maximum (sent only when it changes). */
void oled_flush(const OledGfx &g, float scale, uint8_t drive_pct);
/** Bench: every pixel on (SSD1322 "entire display on") or back to normal. */
void oled_all_on(bool on);
