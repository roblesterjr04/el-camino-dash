#pragma once

#include <stdint.h>

// CAN bus IDs and node numbers (see docs/protocols/can_bus.md). 11-bit standard IDs, 500 kbit/s,
// little-endian payloads. Lower ID = higher priority.

#ifndef ELC_CAN_BITRATE_KBPS
#define ELC_CAN_BITRATE_KBPS 500
#endif

// ---- sensor data, re-sent by the center from the sensor pack UART (one group per UART frame) ----
static const uint16_t ELC_CAN_ID_SENSOR_SPEED = 0x100;   // wheel rpm u32, engine rpm u32
static const uint16_t ELC_CAN_ID_SENSOR_ANALOG = 0x101;  // fuel, engine temp, oil, charge band (u16 ADC each)
static const uint16_t ELC_CAN_ID_SENSOR_STATUS = 0x102;  // ambient u16, flags u8, faults u16, seq u8
static const uint16_t ELC_CAN_ID_SENSOR_ODO = 0x103;     // odometer tenths u32 (0xFFFFFFFF unknown), DTE tenths u32
static const uint16_t ELC_CAN_ID_SENSOR_ECON = 0x104;    // MPG instant x10 u16, MPG trip x10 u16, flags u8

// ---- cluster state, sent by the center on change and every 5 s ----
static const uint16_t ELC_CAN_ID_CLUSTER_STATE = 0x210;  // theme u8, center sub-view u8

// ---- accessory slots: slot n owns 0x400 + 0x40 * n .. + 0x3F ----
static const uint16_t ELC_CAN_ID_SLOT_BASE = 0x400;
static const uint16_t ELC_CAN_SLOT_SPAN = 0x40;
static inline uint16_t elc_can_slot_id(uint8_t slot, uint8_t msg) {
  return (uint16_t)(ELC_CAN_ID_SLOT_BASE + ELC_CAN_SLOT_SPAN * slot + (msg & 0x3F));
}
/** Slot that owns a CAN ID, or -1 when the ID is not in a slot range. */
static inline int elc_can_slot_of(uint16_t id) {
  if (id < ELC_CAN_ID_SLOT_BASE || id >= ELC_CAN_ID_SLOT_BASE + 3 * ELC_CAN_SLOT_SPAN) return -1;
  return (id - ELC_CAN_ID_SLOT_BASE) / ELC_CAN_SLOT_SPAN;
}

// Audio bridge messages (offsets within its slot range).
static const uint8_t ELC_CAN_AUDIO_MSG_TRANSPORT = 0x00;  // elapsed s u16, duration s u16, volume u8, text serial u8, flags u8
static const uint8_t ELC_CAN_AUDIO_MSG_TEXT = 0x01;       // [serial << 4 | chunk] + 7 bytes of "Artist - Title"

// ---- heartbeats: 0x700 + node, 1 Hz ----
static const uint16_t ELC_CAN_ID_HEARTBEAT_BASE = 0x700;  // node type u8, fw version u16, uptime s u32, flags u8

// Node numbers. Slots are 0x10 + slot (slot from the SLOT_ID straps on the DigiDash board).
static const uint8_t ELC_CAN_NODE_CENTER = 0x01;
static const uint8_t ELC_CAN_NODE_SPEEDO = 0x02;  // left screen
static const uint8_t ELC_CAN_NODE_TSPS = 0x03;    // right screen
static const uint8_t ELC_CAN_NODE_SLOT0 = 0x10;
static const uint8_t ELC_CAN_MAX_NODES = 0x20;

enum class ElcCanNodeType : uint8_t { Display = 0, AudioBridge = 1, Unknown = 0xFF };
