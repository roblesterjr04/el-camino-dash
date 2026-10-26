#pragma once

// Everything the card's parts share (main.cpp owns the one instance, console.cpp reads and changes it). The same as
// rev A's, with the FDCAN driver in place of the MCP2515 and no EEPROM settings (nothing is left to set: the CAN
// controller is built in, and the unique ID comes from the chip).
#include <stdint.h>

#include "calib.h"
#include "card_chan.h"
#include "card_net.h"
#include "card_ui.h"
#include "engine.h"
#include "hw_fdcan.h"
#include "odometer.h"

#ifndef CARD_FW_VERSION
#define CARD_FW_VERSION 1
#endif
static const uint8_t kCardNode = 0x04;  // fixed in any slot: the sensor pack

struct Card {
  cal::Calibration cal;
  Odometer odo;
  CalStore store;
  Engine engine;
  EngineOut out;
  CardNet net;
  CardUi ui;
  CardChan chan;
  FdCan can;
  uint32_t uid = 0;
  bool fram = false;          // the chip answered at boot (or since)
  bool cal_from_fram = false;
  bool demo = false;          // `demo on` on the console (rev A's J9 jumper); not kept across a restart
  bool cal_dirty = false;
  uint32_t cal_saved_ms = 0;
  uint32_t center_ms = 0;     // the center was last heard (0xA5, 0x213, 0x701)
  uint32_t identify_until = 0;
  uint32_t steps = 0;
  EngineIn in;                // the last step's inputs
  uint8_t tach_ppr = 4;  // a V8 fires 4 times a revolution
};

extern Card card;

void console_begin();
void console_poll(uint32_t now);
/** Writes the calibration to the FRAM now (key-off, `cal save`). */
bool card_save_calibration();
[[noreturn]] void card_restart();
