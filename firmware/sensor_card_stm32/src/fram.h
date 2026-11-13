#pragma once
#include <stdint.h>

#include "odometer.h"

/** The MB85RC256V on I2C1 (PB6 / PB7) at 0x50, 400 kHz. The core's I2C driver has a 100 ms timeout per transfer, so
 *  a stuck bus costs at most that per call, never a hang. */
void fram_begin();
FramIo fram_io();
