#include "center_i2c_host.h"

#include <Wire.h>

void center_i2c_host_scan(uint8_t *addrs7, int max_n, int *out_n) {
  if (!addrs7 || max_n <= 0 || !out_n) {
    return;
  }
  *out_n = 0;
  for (uint8_t a = 1; a < 127 && *out_n < max_n; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      addrs7[(*out_n)++] = a;
    }
  }
}
