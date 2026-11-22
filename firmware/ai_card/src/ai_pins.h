#pragma once

// ESP32-S3-WROOM-1U-N16R8 pins on the AI card (AICard/generate/gen_sch.py is the source of truth; keep both in step).
// Not used: GPIO0 (BOOT only), 3, 45, 46 (strapping), 26-37 (flash and octal PSRAM), 19 / 20 (native USB).

#define AI_PIN_CAN_TX 9
#define AI_PIN_CAN_RX 10
#define AI_PIN_SLOT_ID 4        // ADC1_CH3: 10k to 3.3 V on the card, the slot's resistor to GND
#define AI_PIN_AUX_SENSE 5      // ADC1_CH4: AUX 12 V through 220k / 47k (2.1 V at 12 V): the buck's INPUT, since the buck waits for it
#define AI_PIN_NTC 8            // ADC1_CH7: 10k NTC (beta 3950) to GND, 10k to 3.3 V
#define AI_PIN_I2S_BCK 6        // rail BCK in (slave)
#define AI_PIN_I2S_WS 7         // rail WS in
#define AI_PIN_I2S_MIC 15       // rail MIC in (the audio card's PCM1808 data)
#define AI_PIN_I2S_RET 16       // RET data out, to the 74LVC125
#define AI_PIN_RET_OE_N 17      // 74LVC125 /OE: low = the buffer drives RET. 100k pull-up on the card: off at power-up
#define AI_PIN_LINK_TX 41       // UART1 TX -> module RXD (header pin 10), 1k in series
#define AI_PIN_LINK_RX 42       // UART1 RX <- module TXD (header pin 8), 1k in series
#define AI_PIN_BUCK_EN 11       // high enables the module's 5 V buck (NMOS pulls the buck's EN low otherwise)
#define AI_PIN_SHUTDOWN_REQ 12  // -> module header pin 31 (GPIO6), 1k in series, 100k pull-down
#define AI_PIN_ALIVE 13         // <- module header pin 29 (GPIO5), 1k in series, 100k pull-down
#define AI_PIN_ATTN 14          // spare, module header pin 33, input
#define AI_PIN_LED_MODULE 1     // green: module running
#define AI_PIN_LED_MIC 2        // amber: the microphone is open (listening)
// GPIO43 / 44 are the debug UART (J5), 19 / 20 the USB-C.

#define AI_NTC_R_FIXED 10000
#define AI_NTC_R0 10000
#define AI_NTC_BETA 3950
#define AI_AUX_PRESENT_MV 1000  // above this AUX 12 V is up (about 6.4 V at the 220k / 47k divider: 17.6 % of the input)
