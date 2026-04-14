#pragma once

#include <stdint.h>

#include "cluster_i2c.h"

// Cluster slave on TwoWire(1) @ CLUSTER_I2C_SDA/SCL — leaves Wire(0) for display/touch PMIC.
bool cluster_i2c_slave_begin(uint8_t addr7, void (*on_reg)(uint8_t reg, uint8_t value, void *ctx), void *ctx);

void cluster_i2c_slave_poll();
