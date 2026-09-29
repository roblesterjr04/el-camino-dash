#pragma once

#include <stdint.h>

// Touch + PMIC + peripherals share one I2C bus (ESP32 is host/master). Scan for devices.
void center_i2c_host_scan(uint8_t *addrs7, int max_n, int *out_n);
