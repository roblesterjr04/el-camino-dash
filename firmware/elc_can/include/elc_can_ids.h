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
static const uint16_t ELC_CAN_ID_CENTER_STATUS = 0x213;    // flags u8, reserved u8, reserved u8 (1 Hz)
// 0x214 reserved (was the Wi-Fi network name)

// ---- requests to accessory cards ----
static const uint16_t ELC_CAN_ID_AUDIO_ART_REQUEST = 0x211;  // slot u8, art id u8, first chunk u16, variant u8 (display -> bridge)
static const uint16_t ELC_CAN_ID_PHONE_COMMAND = 0x215;  // slot u8, command u8, arg u8, from node u8, list serial u8 (console -> bridge)

// ---- the console driving the menu on the interface screen, and that menu's state for the console's OLED ----
static const uint16_t ELC_CAN_ID_MENU_INPUT = 0x216;       // event u8, value s8, from node u8 (console -> interface screen)
static const uint16_t ELC_CAN_ID_MENU_STATE = 0x217;       // open u8, screen u8, page u8, opened by u8, text serial u8
static const uint16_t ELC_CAN_ID_MENU_TEXT_MAIN = 0x218;   // [serial << 4 | chunk] + 7 bytes: the focused item or page
static const uint16_t ELC_CAN_ID_MENU_TEXT_SUB = 0x219;    // [serial << 4 | chunk] + 7 bytes: its value or state

// ---- wall time (center, from GPS) ----
static const uint16_t ELC_CAN_ID_TIME = 0x220;        // UTC epoch s u32, local offset min s16, flags u8, reserved u8
static const uint16_t ELC_CAN_ID_PLACE_TEXT = 0x221;  // reserved (was street name); nothing sends it

// ---- accessory ranges: node 0x10 + n owns 0x400 + 0x40 * n .. + 0x3F (n = 0 .. 11, nodes 0x10 .. 0x1B) ----
// The "slot" in these helpers and in the audio messages is that range index (node - 0x10), not the physical card
// slot on the rev 2 board: a card in physical slot s is node 0x14 + s - 1, range index 4 + s - 1.
static const uint16_t ELC_CAN_ID_SLOT_BASE = 0x400;
static const uint16_t ELC_CAN_SLOT_SPAN = 0x40;
static const uint8_t ELC_CAN_RANGE_COUNT = 12;      // nodes 0x10 .. 0x1B
static const uint8_t ELC_CAN_RANGE_CONSOLE = 3;     // node 0x13: the console's range, never an audio bridge
static inline uint16_t elc_can_slot_id(uint8_t slot, uint8_t msg) {
  return (uint16_t)(ELC_CAN_ID_SLOT_BASE + ELC_CAN_SLOT_SPAN * slot + (msg & 0x3F));
}
/** Range index (node - 0x10) that owns a CAN ID, or -1 when the ID is in no accessory range (or the console's). */
static inline int elc_can_slot_of(uint16_t id) {
  if (id < ELC_CAN_ID_SLOT_BASE || id >= ELC_CAN_ID_SLOT_BASE + ELC_CAN_RANGE_COUNT * ELC_CAN_SLOT_SPAN) return -1;
  const int r = (id - ELC_CAN_ID_SLOT_BASE) / ELC_CAN_SLOT_SPAN;
  return r == ELC_CAN_RANGE_CONSOLE ? -1 : r;
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

// Node numbers (docs/hardware/accessory_ports.md, "Node numbers"). Accessories are 0x10 + index, message range
// 0x400 + 0x40 * index:
//   0x10 .. 0x12  rev 1 carriers (the audio bridge's two straps, GPIO13 / GPIO23, grounded = 1, open = 0x10)
//   0x13          the console (the one fixed accessory number)
//   0x14 .. 0x19  rev 2 slot cards: node = 0x14 + slot - 1, the slot read from the slot ID pin (11)
//   0x1A .. 0x1B  other cable-port devices
static const uint8_t ELC_CAN_NODE_CENTER = 0x01;
static const uint8_t ELC_CAN_NODE_SPEEDO = 0x02;  // left screen
static const uint8_t ELC_CAN_NODE_TSPS = 0x03;    // right screen
static const uint8_t ELC_CAN_NODE_CONSOLE = 0x13;  // OLED + knobs board (ESP32-S3) on cable port J17 (planned); range 0x4C0
static const uint8_t ELC_CAN_NODE_SLOT0 = 0x10;       // rev 1 carriers: 0x10 + strap value
static const uint8_t ELC_CAN_NODE_CARD_SLOT1 = 0x14;  // rev 2: the card in slot 1; slot s is 0x14 + s - 1
static const uint8_t ELC_CAN_CARD_SLOTS = 6;
static const uint8_t ELC_CAN_MAX_NODES = 0x20;

/** Node number of the card in rev 2 slot 1 .. 6, or 0 for anything else (no valid slot). */
static inline uint8_t elc_can_node_for_card_slot(uint8_t slot) {
  return (slot >= 1 && slot <= ELC_CAN_CARD_SLOTS) ? (uint8_t)(ELC_CAN_NODE_CARD_SLOT1 + slot - 1) : 0;
}

/** Rev 2 slot (1 .. 6) from the slot ID pin's voltage in mV, 0 when it is none of them (nothing pulls it down: the
 *  bench, or no main board). The main board pulls pin 11 to GND through 1k, 3.3k, 6.8k, 12k, 22k or 47k against the
 *  card's 10k to 3.3 V: about 300, 819, 1336, 1800, 2269, 2717 mV. The limits are the midpoints; above 2950 mV the pin
 *  is floating. */
static inline uint8_t elc_can_card_slot_from_mv(uint16_t mv) {
  static const uint16_t kUpper[ELC_CAN_CARD_SLOTS] = {560, 1078, 1568, 2035, 2493, 2950};
  for (uint8_t i = 0; i < ELC_CAN_CARD_SLOTS; i++) {
    if (mv < kUpper[i]) return (uint8_t)(i + 1);
  }
  return 0;
}

enum class ElcCanNodeType : uint8_t { Display = 0, AudioBridge = 1, Console = 2, Unknown = 0xFF };

// Console: node 0x13, so it owns 0x4C0..0x4FF, the fourth accessory range. elc_can_slot_of() never returns its range,
// so the audio decoders never see it.
static const uint16_t ELC_CAN_ID_CONSOLE_BASE = 0x4C0;
