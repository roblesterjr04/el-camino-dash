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
// theme u8, center sub-view u8, interface screen u8, media screen u8, brightness % u8, flags u8
static const uint16_t ELC_CAN_ID_CLUSTER_STATE = 0x210;

// Screens, for the interface (menu) and media settings.
static const uint8_t ELC_CAN_SCREEN_LEFT = 0;    // speedometer
static const uint8_t ELC_CAN_SCREEN_CENTER = 1;
static const uint8_t ELC_CAN_SCREEN_RIGHT = 2;   // fuel / temp / oil / amp

// ---- settings: the center owns them (NVS + SD); anyone asks, the center applies and re-sends 0x210 ----
static const uint16_t ELC_CAN_ID_SETTING_REQUEST = 0x212;  // setting u8, value u8, from node u8
static const uint16_t ELC_CAN_ID_CENTER_STATUS = 0x213;    // flags u8, Wi-Fi RSSI s8, text serial u8 (1 Hz)
static const uint16_t ELC_CAN_ID_CENTER_TEXT = 0x214;      // [serial << 4 | chunk] + 7 bytes: Wi-Fi network name

// ---- requests to accessory cards ----
static const uint16_t ELC_CAN_ID_AUDIO_ART_REQUEST = 0x211;  // slot u8, art id u8, first chunk u16, variant u8 (display -> bridge)
static const uint16_t ELC_CAN_ID_PHONE_COMMAND = 0x215;  // slot u8, command u8, arg u8, from node u8, list serial u8 (console -> bridge)

// ---- the console driving the menu on the interface screen, and that menu's state for the console's OLED ----
static const uint16_t ELC_CAN_ID_MENU_INPUT = 0x216;       // event u8, value s8, from node u8 (console -> interface screen)
static const uint16_t ELC_CAN_ID_MENU_STATE = 0x217;       // open u8, screen u8, page u8, opened by u8, text serial u8
static const uint16_t ELC_CAN_ID_MENU_TEXT_MAIN = 0x218;   // [serial << 4 | chunk] + 7 bytes: the focused item or page
static const uint16_t ELC_CAN_ID_MENU_TEXT_SUB = 0x219;    // [serial << 4 | chunk] + 7 bytes: its value or state

// ---- wall time and place (center, from GPS / NTP) ----
static const uint16_t ELC_CAN_ID_TIME = 0x220;        // UTC epoch s u32, local offset min s16, flags u8, place serial u8
static const uint16_t ELC_CAN_ID_PLACE_TEXT = 0x221;  // [serial << 4 | chunk] + 7 bytes: street / place name

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
static const uint8_t ELC_CAN_AUDIO_MSG_ART_INFO = 0x02;   // art id u8, size bytes u32, variant u8 (answers a request)
static const uint8_t ELC_CAN_AUDIO_MSG_ART_DATA = 0x03;   // art id u8, variant << 14 | chunk u16, 5 bytes of image
static const uint8_t ELC_CAN_AUDIO_MSG_PHONE_STATUS = 0x04;   // flags, call state, call s u16, signal/battery, serials, list, volume
static const uint8_t ELC_CAN_AUDIO_MSG_PHONE_TEXT = 0x05;     // kind u8, [serial << 4 | chunk] u8, 6 bytes of text
static const uint8_t ELC_CAN_AUDIO_MSG_PHONE_PAIRING = 0x06;  // passkey u32, pairing seconds left u8

// ---- heartbeats: 0x700 + node, 1 Hz ----
static const uint16_t ELC_CAN_ID_HEARTBEAT_BASE = 0x700;  // node type u8, fw version u16, uptime s u32, flags u8

// Node numbers. Accessories (carrier boards on the DigiDash accessory ports, docs/hardware/accessory_ports.md) are
// 0x10 + index, message range 0x400 + 0x40 * index. The index comes from the carrier itself: the audio bridge reads
// two straps (GPIO13 / GPIO23, grounded = 1, open = 0), so a carrier with none fitted is 0x10.
static const uint8_t ELC_CAN_NODE_CENTER = 0x01;
static const uint8_t ELC_CAN_NODE_SPEEDO = 0x02;  // left screen
static const uint8_t ELC_CAN_NODE_TSPS = 0x03;    // right screen
static const uint8_t ELC_CAN_NODE_CONSOLE = 0x13;  // OLED + knobs board (ESP32-S3) on cable port J17 (planned); range 0x4C0
static const uint8_t ELC_CAN_NODE_SLOT0 = 0x10;
static const uint8_t ELC_CAN_MAX_NODES = 0x20;

enum class ElcCanNodeType : uint8_t { Display = 0, AudioBridge = 1, Console = 2, Unknown = 0xFF };

// Console (planned): node 0x13, so it owns 0x4C0..0x4FF, the fourth accessory range. Messages defined with its
// firmware. The audio decoders only look at the first three ranges (elc_can_slot_of), so they never see it.
static const uint16_t ELC_CAN_ID_CONSOLE_BASE = 0x4C0;
