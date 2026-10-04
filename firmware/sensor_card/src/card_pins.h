#pragma once

// The sensor card's pins (SensorCard/DATASHEET.md, MCU pin map). They are the ATmega2560's pins numbered the way an
// Arduino Mega 2560 numbers them, so the same image runs on a Mega on the bench.
#include <stdint.h>

static const uint8_t PIN_CAN_CS = 53;      // PB0 (SS): MCP2515 chip select on the card
static const uint8_t PIN_CAN_INT = 2;      // PE4 (INT4): MCP2515 INT (the driver polls; it is not needed)
static const uint8_t PIN_LED = 13;         // PB7: status LED, high = on
static const uint8_t PIN_TACH = 49;        // PL0 ICP4: tach, falling edge = spark
static const uint8_t PIN_SPEED = 48;       // PL1 ICP5: speed, falling edge
static const uint8_t PIN_DEMO = 37;        // PC0: J9, high = demo sweep (10k pull-down on the card)
static const uint8_t PIN_PWR_HOLD = 36;    // PC1: drives WAKE to hold the main board's 5 V rail
static const uint8_t PIN_RPM_SEL_B = 29;   // PA7: J5 3-4, low = fitted
static const uint8_t PIN_RPM_SEL_A = 28;   // PA6: J5 1-2, low = fitted
static const uint8_t PIN_PRESS_MON = 26;   // PA4: OILP V feed monitor, high = OK
static const uint8_t PIN_TEMP_MON = 25;    // PA3: ETEMP V feed monitor, high = OK
static const uint8_t PIN_IGN = 23;         // PA1: ignition (slot switched 12 V), high = on
static const uint8_t PIN_ALT_L = 22;       // PA0: alternator L, high = charging

// ADC channels
static const uint8_t ADC_FUEL = 0, ADC_TEMP = 1, ADC_OIL = 2, ADC_AMB = 3, ADC_BATT = 8, ADC_WAKE = 9;

// MCP2515 chip-select candidates, tried in this order when none is set: the card's own, then the ones CAN shields use
static const uint8_t kCsCandidates[3] = {53, 10, 9};
