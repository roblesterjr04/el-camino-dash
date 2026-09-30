#include "oled_ssd1322.h"

#include <Arduino.h>
#include <SPI.h>

#include "console_pins.h"
#include "oled_gfx.h"

#ifndef CONSOLE_OLED_ROTATE_180
#define CONSOLE_OLED_ROTATE_180 1  // the module is mounted upside down
#endif

namespace {

// The panel's 256 columns are SSD1322 column addresses 0x1C..0x5B (4 pixels each), as in U8g2's NHD driver.
constexpr uint8_t kColStart = 0x1C;
uint8_t s_frame[OledGfx::W * OledGfx::H / 2];
const SPISettings kSpi(CONSOLE_OLED_SPI_HZ, MSBFIRST, SPI_MODE0);

void cmd(uint8_t c) {
  digitalWrite(CONSOLE_OLED_DC_PIN, LOW);
  SPI.transfer(c);
}

void arg(uint8_t a) {
  digitalWrite(CONSOLE_OLED_DC_PIN, HIGH);
  SPI.transfer(a);
}

void begin_tx() {
  SPI.beginTransaction(kSpi);
  digitalWrite(CONSOLE_OLED_CS_PIN, LOW);
}

void end_tx() {
  digitalWrite(CONSOLE_OLED_CS_PIN, HIGH);
  SPI.endTransaction();
}

}  // namespace

void oled_begin() {
  pinMode(CONSOLE_OLED_CS_PIN, OUTPUT);
  pinMode(CONSOLE_OLED_DC_PIN, OUTPUT);
  digitalWrite(CONSOLE_OLED_CS_PIN, HIGH);
  SPI.begin(CONSOLE_OLED_SCK_PIN, -1, CONSOLE_OLED_MOSI_PIN, -1);
  if (CONSOLE_OLED_RST_PIN >= 0) {
    pinMode(CONSOLE_OLED_RST_PIN, OUTPUT);
    digitalWrite(CONSOLE_OLED_RST_PIN, HIGH);
    delay(5);
    digitalWrite(CONSOLE_OLED_RST_PIN, LOW);
    delay(50);  // generous: some modules need far more than the datasheet's 2 µs
    digitalWrite(CONSOLE_OLED_RST_PIN, HIGH);
  }
  delay(100);
  begin_tx();
  // U8g2's SSD1322 NHD 256×64 sequence (the one this module works with).
  cmd(0xFD); arg(0x12);              // unlock
  cmd(0xAE);                         // display off
  cmd(0xB3); arg(0x91);              // clock
  cmd(0xCA); arg(0x3F);              // multiplex 1/64
  cmd(0xA2); arg(0x00);              // display offset
  cmd(0xA1); arg(0x00);              // start line
  cmd(0xA0); arg(0x06); arg(0x11);   // remap: rows top to bottom, left pixel in the high nibble
  cmd(0xAB); arg(0x01);              // internal VDD regulator
  cmd(0xB4); arg(0xA0); arg(0xFD);   // display enhancement A
  cmd(0xC1); arg(0x9F);              // contrast
  cmd(0xC7); arg(0x0F);              // master current
  cmd(0xB9);                         // linear grey scale
  cmd(0xB1); arg(0xE2);              // phase lengths
  cmd(0xD1); arg(0xA2); arg(0x20);   // display enhancement B
  cmd(0xBB); arg(0x1F);              // precharge voltage
  cmd(0xB6); arg(0x08);              // second precharge
  cmd(0xBE); arg(0x07);              // VCOMH
  cmd(0xA6);                         // normal display
  cmd(0xA9);                         // exit partial display
  end_tx();
  static OledGfx blank;  // 16 KB: never on the loop task's stack
  oled_flush(blank, 1.0f);
  begin_tx();
  cmd(0xAF);  // display on
  end_tx();
}

void oled_flush(const OledGfx &g, float scale) {
  const uint8_t *px = g.pixels();
  if (scale < 0) scale = 0;
  if (scale > 1) scale = 1;
  const int k = (int)(scale * 256 + 0.5f);
  uint8_t lut[16];
  for (int i = 0; i < 16; i++) lut[i] = (uint8_t)((i * k + 128) >> 8);
  const int n = OledGfx::W * OledGfx::H;
  for (int i = 0; i < n; i += 2) {
    // Rotated 180°: output pixel i is source pixel n-1-i.
    const uint8_t a = CONSOLE_OLED_ROTATE_180 ? px[n - 1 - i] : px[i];
    const uint8_t b = CONSOLE_OLED_ROTATE_180 ? px[n - 2 - i] : px[i + 1];
    s_frame[i / 2] = (uint8_t)((lut[a & 15] << 4) | lut[b & 15]);
  }
  begin_tx();
  cmd(0x15); arg(kColStart); arg(kColStart + 63);  // columns
  cmd(0x75); arg(0); arg(63);                       // rows
  cmd(0x5C);                                        // write RAM
  digitalWrite(CONSOLE_OLED_DC_PIN, HIGH);
  SPI.writeBytes(s_frame, sizeof(s_frame));
  end_tx();
}

void oled_all_on(bool on) {
  begin_tx();
  cmd(on ? 0xA5 : 0xA6);
  end_tx();
}
