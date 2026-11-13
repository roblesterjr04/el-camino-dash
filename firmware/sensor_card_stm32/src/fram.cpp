#include "fram.h"

#include <Arduino.h>
#include <Wire.h>

#include "card_pins.h"

static const uint8_t kAddr = 0x50;
static const uint8_t kChunk = 24;  // Wire's buffer is 32 bytes, two of them the address
static TwoWire s_wire(PIN_FRAM_SDA, PIN_FRAM_SCL);

static bool probe(void *) {
  s_wire.beginTransmission(kAddr);
  return s_wire.endTransmission() == 0;
}

static bool read_chunk(uint16_t addr, uint8_t *buf, uint8_t len) {
  while (s_wire.available() > 0) (void)s_wire.read();
  s_wire.beginTransmission(kAddr);
  s_wire.write((uint8_t)(addr >> 8));
  s_wire.write((uint8_t)addr);
  if (s_wire.endTransmission(false) != 0) return false;
  if (s_wire.requestFrom(kAddr, (size_t)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) {
    const int v = s_wire.read();
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
    s_wire.beginTransmission(kAddr);
    s_wire.write((uint8_t)(addr >> 8));
    s_wire.write((uint8_t)addr);
    for (uint8_t i = 0; i < n; i++) s_wire.write(buf[i]);
    if (s_wire.endTransmission() != 0) return false;
    addr += n;
    buf += n;
    len -= n;
  }
  return true;
}

void fram_begin() {
  s_wire.begin();
  s_wire.setClock(400000);
}

FramIo fram_io() { return FramIo{probe, fram_read, fram_write, nullptr}; }
