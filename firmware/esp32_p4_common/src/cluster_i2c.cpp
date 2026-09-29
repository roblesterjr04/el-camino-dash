#include "cluster_i2c.h"

#include <Arduino.h>
#include <Wire.h>

#if defined(ARDUINO_ARCH_ESP32)

static TwoWire s_cluster_wire(1);
static bool s_cluster_ready = false;
static bool s_ack_speedo = false;
static bool s_ack_tsps = false;
static bool s_ack_audio = false;

bool cluster_i2c_master_begin() {
  if (!s_cluster_ready) {
    s_cluster_wire.begin(CLUSTER_I2C_SDA, CLUSTER_I2C_SCL, 100000u);
    s_cluster_wire.setTimeOut(20);  // ms: an unpowered/stuck slave must not stall the render loop
    s_cluster_ready = true;
  }
  return s_cluster_ready;
}

bool cluster_i2c_master_write_reg(uint8_t addr7, uint8_t reg, uint8_t value) {
  if (!cluster_i2c_master_begin()) {
    return false;
  }
  s_cluster_wire.beginTransmission(addr7);
  s_cluster_wire.write(reg);
  s_cluster_wire.write(value);
  uint8_t e = s_cluster_wire.endTransmission();
  if (e != 0) {
    s_cluster_wire.beginTransmission(addr7);
    s_cluster_wire.write(reg);
    s_cluster_wire.write(value);
    e = s_cluster_wire.endTransmission();
  }
  return e == 0;
}

bool cluster_i2c_master_read_reg(uint8_t addr7, uint8_t reg, uint8_t *value) {
  if (!value || !cluster_i2c_master_begin()) {
    return false;
  }
  s_cluster_wire.beginTransmission(addr7);
  s_cluster_wire.write(reg);
  if (s_cluster_wire.endTransmission(false) != 0) {
    return false;
  }
  if (s_cluster_wire.requestFrom((int)addr7, 1) != 1) {
    return false;
  }
  *value = (uint8_t)s_cluster_wire.read();
  return true;
}

bool cluster_i2c_broadcast_center_theme(ClusterTheme theme, ClusterCenterSubview center_sub) {
  if (!cluster_i2c_master_begin()) {
    return false;
  }
  const uint8_t th = (uint8_t)theme;
  const uint8_t cs = (uint8_t)center_sub;
  s_ack_speedo = cluster_i2c_master_write_reg(CLUSTER_I2C_ADDR_SPEEDO, CLUSTER_REG_THEME, th) &&
                 cluster_i2c_master_write_reg(CLUSTER_I2C_ADDR_SPEEDO, CLUSTER_REG_CENTER_SUBVIEW, cs);
  s_ack_tsps = cluster_i2c_master_write_reg(CLUSTER_I2C_ADDR_TSPS, CLUSTER_REG_THEME, th) &&
               cluster_i2c_master_write_reg(CLUSTER_I2C_ADDR_TSPS, CLUSTER_REG_CENTER_SUBVIEW, cs);
#if defined(CENTER_AUDIO_I2C) && CENTER_AUDIO_I2C
  // Bench only: in the car the audio bridge hears the theme on CAN (0x210).
  s_ack_audio = cluster_i2c_master_write_reg(CLUSTER_I2C_ADDR_ESP32D_THEME, CLUSTER_REG_THEME, th);
#endif
  return s_ack_speedo && s_ack_tsps;
}

bool cluster_i2c_last_ack(uint8_t addr7) {
  if (addr7 == CLUSTER_I2C_ADDR_SPEEDO) return s_ack_speedo;
  if (addr7 == CLUSTER_I2C_ADDR_TSPS) return s_ack_tsps;
  if (addr7 == CLUSTER_I2C_ADDR_ESP32D_THEME) return s_ack_audio;
  return false;
}

#else

bool cluster_i2c_last_ack(uint8_t) { return false; }

bool cluster_i2c_master_begin() { return false; }

bool cluster_i2c_master_write_reg(uint8_t, uint8_t, uint8_t) { return false; }

bool cluster_i2c_master_read_reg(uint8_t, uint8_t, uint8_t *) { return false; }

bool cluster_i2c_broadcast_center_theme(ClusterTheme, ClusterCenterSubview) { return false; }

#endif
