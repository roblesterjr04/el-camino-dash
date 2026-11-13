#pragma once

// The rev B card's pins (SensorCard2/README.md, pin map): STM32G0B1KBU6, UFQFPN-32. Names are the STM32duino port
// pin names (PA0 ...), which the generic variant defines as Arduino pin numbers.
#include <Arduino.h>
#include <stdint.h>

static const uint32_t PIN_LED = PB5;         // status LED, high = on
static const uint32_t PIN_PWR_HOLD = PA15;   // drives WAKE through D1 to hold the main board's 5 V rail
static const uint32_t PIN_RPM_SEL_A = PC6;   // J5 1-2, low = fitted (internal pull-up)
static const uint32_t PIN_RPM_SEL_B = PB3;   // J5 3-4, low = fitted (internal pull-up)
static const uint32_t PIN_TEMP_MON = PB2;    // ETEMP V feed present (10k / 15k from the 5 V feed), high = OK
static const uint32_t PIN_PRESS_MON = PB9;   // OILP V feed present, high = OK
static const uint32_t PIN_TACH = PA8;        // TIM1_CH1 (AF2), falling edge = spark (the opto pulls it low)
static const uint32_t PIN_SPEED = PB4;       // TIM3_CH1 (AF1), falling edge
static const uint32_t PIN_J8_TX = PA2;       // USART2: the sensor frames out (through U7 to 5 V)
static const uint32_t PIN_J8_RX = PA3;
static const uint32_t PIN_FRAM_SDA = PB7;    // I2C1
static const uint32_t PIN_FRAM_SCL = PB6;
// USART1 (PA9 / PA10, J7) is `Serial`: SERIAL_UART_INSTANCE / PIN_SERIAL_* in platformio.ini.
// FDCAN1 is PA11 (RX) / PA12 (TX), AF3: hw_fdcan.cpp. SWD is PA13 / PA14 (J6).

// ADC1 inputs (hw_adc.cpp scans them in this, ascending, order)
static const uint8_t ADC_FUEL = 0, ADC_TEMP = 1, ADC_OIL = 4, ADC_AMB = 5, ADC_ALT = 6, ADC_BATT = 7, ADC_WAKE = 8,
                     ADC_IGN = 9;

// The level inputs' dividers (top / bottom, kohm) and the slot voltages they switch at, the same as rev A's
static const float kIgnDivTop = 33.0f, kIgnDivBottom = 10.0f, kIgnOnVolts = 7.5f, kIgnOffVolts = 3.75f;
static const float kAltDivTop = 39.0f, kAltDivBottom = 10.0f, kAltOnVolts = 9.6f, kAltOffVolts = 4.8f;
static const float kBattDivTop = 100.0f, kBattDivBottom = 22.0f;
