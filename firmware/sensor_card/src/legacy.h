#pragma once

// The frames the screens already understand: CAN 0x100-0x104 (the center's re-send format) and the 36-byte sensor UART
// frame (docs/protocols/sensor_uart.md, v4). No hardware: host-tested.
#include <stddef.h>
#include <stdint.h>

#include "elc_can_codec.h"
#include "engine.h"

/** The five CAN frames for one step (0x100..0x104). `seq` bumps once per step. */
void legacy_can_frames(const EngineOut &o, uint8_t seq, ElcCanFrame out[ELC_CAN_SENSOR_FRAMES]);

/** The 36-byte UART frame: AA 55, 33 bytes big-endian, XOR of the 33. */
static const size_t kUartFrameLen = 36;
void legacy_uart_frame(const EngineOut &o, uint8_t out[kUartFrameLen]);
