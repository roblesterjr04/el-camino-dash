#include "p4_touch.h"

#include <Arduino.h>
#include <Wire.h>

#ifndef P4_TOUCH_ENABLE
#define P4_TOUCH_ENABLE 1
#endif

#ifndef P4_TOUCH_I2C_HZ
#define P4_TOUCH_I2C_HZ 100000
#endif

#if P4_TOUCH_ENABLE

enum class P4TouchChip : uint8_t { Unknown = 0, Gt911, Cst816 };

static P4TouchChip s_chip = P4TouchChip::Unknown;
static uint8_t s_addr = 0;
static bool s_was_down;
static int16_t s_last_x;
static int16_t s_last_y;

static bool wire_read_reg(uint8_t addr7, uint16_t reg16_be, uint8_t *buf, size_t len) {
  Wire.beginTransmission(addr7);
  Wire.write((uint8_t)(reg16_be >> 8));
  Wire.write((uint8_t)(reg16_be & 0xFF));
  if (Wire.endTransmission() != 0) {
    return false;
  }
  size_t n = Wire.requestFrom((int)addr7, (int)len);
  if (n < len) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    buf[i] = Wire.read();
  }
  return true;
}

static bool wire_write8(uint8_t addr7, uint16_t reg16_be, uint8_t val) {
  Wire.beginTransmission(addr7);
  Wire.write((uint8_t)(reg16_be >> 8));
  Wire.write((uint8_t)(reg16_be & 0xFF));
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool gt911_probe(uint8_t addr7) {
  uint8_t b[4];
  if (!wire_read_reg(addr7, 0x8140, b, sizeof(b))) {
    return false;
  }
  (void)b;
  return true;
}

static bool cst816_read(int16_t *x, int16_t *y, bool *down) {
  const uint8_t addr7 = 0x15;
  Wire.beginTransmission(addr7);
  Wire.write(0x01);
  if (Wire.endTransmission() != 0) {
    *down = false;
    return false;
  }
  if (Wire.requestFrom((int)addr7, 6) < 6) {
    *down = false;
    return false;
  }
  uint8_t raw[6];
  for (int i = 0; i < 6; i++) {
    raw[i] = Wire.read();
  }
  *down = (raw[1] > 0);
  *x = (int16_t)(((raw[2] & 0x0F) << 8) | raw[3]);
  *y = (int16_t)(((raw[4] & 0x0F) << 8) | raw[5]);
  return true;
}

static bool gt911_read_points(int16_t *x, int16_t *y, bool *down) {
  uint8_t st = 0;
  if (!wire_read_reg(s_addr, 0x814E, &st, 1)) {
    *down = false;
    return false;
  }
  if ((st & 0x80) == 0) {
    *down = false;
    return true;
  }
  uint8_t n = st & 0x0F;
  if (n == 0 || n > 5) {
    wire_write8(s_addr, 0x814E, 0);
    *down = false;
    return true;
  }
  uint8_t buf[8];
  if (!wire_read_reg(s_addr, 0x814F, buf, 8)) {
    *down = false;
    return false;
  }
  // Registers 8150–8153: X; 8152–8153: Y (16-bit LE per Goodix / esp_lcd_touch_gt911)
  uint16_t xi = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
  uint16_t yi = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
  *x = (int16_t)xi;
  *y = (int16_t)yi;
  *down = true;
  wire_write8(s_addr, 0x814E, 0);
  return true;
}

static bool sample_finger(int16_t *x, int16_t *y, bool *down) {
  if (s_chip == P4TouchChip::Gt911) {
    return gt911_read_points(x, y, down);
  }
  if (s_chip == P4TouchChip::Cst816) {
    return cst816_read(x, y, down);
  }
  *down = false;
  return false;
}

void p4_touch_begin(int sda, int scl) {
  Wire.begin(sda, scl);
  Wire.setClock((uint32_t)P4_TOUCH_I2C_HZ);
  delay(50);

  s_chip = P4TouchChip::Unknown;
  s_addr = 0;
  if (gt911_probe(0x5D)) {
    s_chip = P4TouchChip::Gt911;
    s_addr = 0x5D;
  } else if (gt911_probe(0x14)) {
    s_chip = P4TouchChip::Gt911;
    s_addr = 0x14;
  } else {
    int16_t tx = 0, ty = 0;
    bool d = false;
    if (cst816_read(&tx, &ty, &d)) {
      (void)tx;
      (void)ty;
      (void)d;
      s_chip = P4TouchChip::Cst816;
      s_addr = 0x15;
    }
  }
  s_was_down = false;
}

bool p4_touch_finger_down() {
  int16_t x = 0, y = 0;
  bool down = false;
  sample_finger(&x, &y, &down);
  return down;
}

void p4_touch_sample(int16_t *x, int16_t *y, bool *down) {
  if (!x || !y || !down) {
    return;
  }
  sample_finger(x, y, down);
}

bool p4_touch_poll_release(int16_t *x, int16_t *y) {
  int16_t tx = 0;
  int16_t ty = 0;
  bool down = false;
  sample_finger(&tx, &ty, &down);
  if (down) {
    s_last_x = tx;
    s_last_y = ty;
  }
  bool released = false;
  if (s_was_down && !down) {
    released = true;
    *x = s_last_x;
    *y = s_last_y;
  }
  s_was_down = down;
  return released;
}

#else

void p4_touch_begin(int, int) {}

bool p4_touch_finger_down() { return false; }

void p4_touch_sample(int16_t *x, int16_t *y, bool *down) {
  if (x) {
    *x = 0;
  }
  if (y) {
    *y = 0;
  }
  if (down) {
    *down = false;
  }
}

bool p4_touch_poll_release(int16_t *x, int16_t *y) {
  (void)x;
  (void)y;
  return false;
}

#endif
