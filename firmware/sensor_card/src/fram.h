#pragma once
#include <stdint.h>

#include "odometer.h"

/** The MB85RC256V on I2C 0x50 (TWI, 400 kHz, with a timeout so a stuck bus cannot freeze the loop). */
void fram_begin();
FramIo fram_io();
