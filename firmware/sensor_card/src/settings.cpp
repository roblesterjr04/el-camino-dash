#include "settings.h"

#include <Arduino.h>
#include <EEPROM.h>

static const uint8_t kMagic = 0xE5, kVersion = 1, kLen = 9;

static uint8_t crc8(const uint8_t *p, uint8_t n) {
  uint8_t c = 0;
  for (uint8_t i = 0; i < n; i++) {
    c ^= p[i];
    for (uint8_t b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

void settings_load(CardSettings &s) {
  uint8_t b[kLen];
  for (uint8_t i = 0; i < kLen; i++) b[i] = EEPROM.read(i);
  if (b[0] != kMagic || b[1] != kVersion || crc8(b, kLen - 1) != b[kLen - 1]) return;  // blank or damaged: defaults
  s.can_cs = b[2];
  s.can_mhz = b[3] == 8 ? 8 : 16;
  s.uid = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
}

void settings_save(const CardSettings &s) {
  const uint8_t b[kLen] = {kMagic, kVersion, s.can_cs, s.can_mhz, (uint8_t)s.uid, (uint8_t)(s.uid >> 8),
                           (uint8_t)(s.uid >> 16), (uint8_t)(s.uid >> 24), 0};
  uint8_t w[kLen];
  for (uint8_t i = 0; i < kLen; i++) w[i] = b[i];
  w[kLen - 1] = crc8(w, kLen - 1);
  for (uint8_t i = 0; i < kLen; i++) EEPROM.update(i, w[i]);
}
