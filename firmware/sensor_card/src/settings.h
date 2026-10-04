#pragma once
#include <stdint.h>

/** What the card keeps in the ATmega's own EEPROM: the CAN controller's chip select and crystal (so a CAN shield on a
 *  Mega can be told once), and a unique ID for a card with no FRAM. Everything of the car's lives in the FRAM. */
struct CardSettings {
  uint8_t can_cs = 0;      // 0 = find it (53, 10, 9)
  uint8_t can_mhz = 16;    // the MCP2515's crystal: 16 (the card) or 8 (some shields)
  uint32_t uid = 0;        // 0 = none made yet
};

void settings_load(CardSettings &s);
void settings_save(const CardSettings &s);
