#include "fram.h"

#include <Arduino.h>
#include <Wire.h>

static const uint8_t kAddr = 0x50;
static const uint8_t kChunk = 24;  // Wire's buffer is 32 bytes, two of them the address

static bool probe(void *) {
  Wire.beginTransmission(kAddr);
  return Wire.endTransmission() == 0;
}

static bool read_chunk(uint16_t addr, uint8_t *buf, uint8_t len) {
  while (Wire.available() > 0) (void)Wire.read();
  Wire.beginTransmission(kAddr);
  Wire.write((uint8_t)(addr >> 8));
  Wire.write((uint8_t)addr);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)kAddr, (int)len) != (int)len) return false;
  for (uint8_t i = 0; i < len; i++) {
    const int v = Wire.read();
    if (v < 0) return false;
    buf[i] = (uint8_t)v;
  }
  return true;
}

static bool fram_read(void *, uint16_t addr, uint8_t *buf, uint8_t len) {
  while (len) {
    const uint8_t n = len > kChunk ? kChunk : len;
    if (!read_chunk(addr, buf, n)) return false;
    addr += n;
    buf += n;
    len -= n;
  }
  return true;
}

static bool fram_write(void *, uint16_t addr, const uint8_t *buf, uint8_t len) {
  while (len) {
    const uint8_t n = len > kChunk ? kChunk : len;
    Wire.beginTransmission(kAddr);
    Wire.write((uint8_t)(addr >> 8));
    Wire.write((uint8_t)addr);
    for (uint8_t i = 0; i < n; i++) Wire.write(buf[i]);
    if (Wire.endTransmission() != 0) return false;
    addr += n;
    buf += n;
    len -= n;
  }
  return true;
}

void fram_begin() {
  Wire.begin();
  Wire.setClock(400000);
  Wire.setWireTimeout(3000 /* us */, true /* reset the bus on a timeout */);
}

FramIo fram_io() { return FramIo{probe, fram_read, fram_write, nullptr}; }
