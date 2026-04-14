#include "center_audio_link.h"

#include <Arduino.h>
#include <Wire.h>

#ifndef CENTER_AUDIO_I2C_ADDR
#define CENTER_AUDIO_I2C_ADDR 0x2A
#endif
#ifndef CENTER_AUDIO_REG_METADATA
#define CENTER_AUDIO_REG_METADATA 0x01
#endif
#ifndef CENTER_AUDIO_POLL_MS
#define CENTER_AUDIO_POLL_MS 1600u
#endif

static char s_line[40];
static bool s_ok;
static uint32_t s_last_poll;

static uint16_t elc565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | ((uint16_t)b >> 3));
}

void center_audio_link_begin() {
  s_line[0] = '\0';
  s_ok = false;
  s_last_poll = 0;
}

void center_audio_link_poll() {
  const uint32_t now = millis();
  if (now - s_last_poll < CENTER_AUDIO_POLL_MS) {
    return;
  }
  s_last_poll = now;

  Wire.beginTransmission(CENTER_AUDIO_I2C_ADDR);
  Wire.write(CENTER_AUDIO_REG_METADATA);
  if (Wire.endTransmission(false) != 0) {
    s_ok = false;
    s_line[0] = '\0';
    return;
  }

  const int n = Wire.requestFrom((int)CENTER_AUDIO_I2C_ADDR, 33);
  if (n <= 0) {
    s_ok = false;
    s_line[0] = '\0';
    return;
  }

  const int len_byte = Wire.read();
  if (len_byte < 0) {
    s_ok = false;
    s_line[0] = '\0';
    return;
  }

  size_t len = (size_t)len_byte;
  if (len > 32) {
    len = 32;
  }
  size_t i = 0;
  for (; i < len && Wire.available(); i++) {
    const int c = Wire.read();
    if (c < 0) {
      break;
    }
    s_line[i] = (char)c;
  }
  s_line[i] = '\0';
  while (Wire.available()) {
    (void)Wire.read();
  }
  s_ok = true;
}

bool center_audio_link_ok() { return s_ok; }

const char *center_audio_link_line() { return s_line; }

void center_audio_link_draw(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h) {
  if (!gfx || !s_ok || s_line[0] == '\0') {
    return;
  }

  gfx->setFont(nullptr);
  gfx->setTextSize(1);
  int16_t x0 = 0, y0 = 0;
  uint16_t tw = 0, th = 0;
  gfx->getTextBounds(s_line, 0, 0, &x0, &y0, &tw, &th);
  const int16_t pad_x = 10;
  const int16_t pad_y = 6;
  const int16_t bar_h = (int16_t)(th + 2 * pad_y);
  const int16_t bar_y = (int16_t)(screen_h - bar_h - 6);
  if (bar_y < 0) {
    return;
  }
  const int16_t bar_w = screen_w;
  gfx->fillRect(0, bar_y, bar_w, bar_h, elc565(12, 12, 18));
  gfx->drawFastHLine(0, bar_y, bar_w, elc565(55, 55, 70));

  gfx->setTextColor(elc565(210, 210, 220));
  const int16_t tx = (int16_t)((screen_w - (int16_t)tw) / 2);
  const int16_t ty = (int16_t)(bar_y + pad_y - (int16_t)y0);
  gfx->setCursor(tx, ty);
  gfx->print(s_line);
}
