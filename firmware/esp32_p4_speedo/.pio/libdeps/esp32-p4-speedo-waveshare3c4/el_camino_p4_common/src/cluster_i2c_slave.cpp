#include "cluster_i2c_slave.h"

#include <Arduino.h>

#if defined(ARDUINO_ARCH_ESP32)

#include <Wire.h>

static TwoWire s_cluster_wire(1);
static void (*s_cb)(uint8_t, uint8_t, void *) = nullptr;
static void *s_ctx = nullptr;

static void on_receive(int num_bytes) {
  if (num_bytes < 2 || !s_cb) {
    while (s_cluster_wire.available()) {
      (void)s_cluster_wire.read();
    }
    return;
  }
  const uint8_t reg = s_cluster_wire.read();
  const uint8_t val = s_cluster_wire.read();
  while (s_cluster_wire.available()) {
    (void)s_cluster_wire.read();
  }
  s_cb(reg, val, s_ctx);
}

bool cluster_i2c_slave_begin(uint8_t addr7, void (*on_reg)(uint8_t, uint8_t, void *), void *ctx) {
  s_cb = on_reg;
  s_ctx = ctx;
  s_cluster_wire.begin(addr7, CLUSTER_I2C_SDA, CLUSTER_I2C_SCL, 100000u);
  s_cluster_wire.onReceive(on_receive);
  return true;
}

void cluster_i2c_slave_poll() {}

#else

bool cluster_i2c_slave_begin(uint8_t, void (*)(uint8_t, uint8_t, void *), void *) { return false; }

void cluster_i2c_slave_poll() {}

#endif
