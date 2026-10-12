#pragma once

// Console board pins (ConsoleBoardRS/README.md, "Pins") and input tuning. Every one can be overridden with -D.

#ifndef CONSOLE_OLED_SCK_PIN
#define CONSOLE_OLED_SCK_PIN 12
#endif
#ifndef CONSOLE_OLED_MOSI_PIN
#define CONSOLE_OLED_MOSI_PIN 11
#endif
#ifndef CONSOLE_OLED_CS_PIN
#define CONSOLE_OLED_CS_PIN 10
#endif
#ifndef CONSOLE_OLED_DC_PIN
#define CONSOLE_OLED_DC_PIN 9
#endif
#ifndef CONSOLE_OLED_RST_PIN
#define CONSOLE_OLED_RST_PIN 8
#endif
#ifndef CONSOLE_OLED_SPI_HZ
#define CONSOLE_OLED_SPI_HZ 10000000
#endif

// The knobs' RJ9 jacks: contacts 2 and 3 on ADC1, 33k pull-ups. Which contact carries the encoder and which the
// switches is found at run time (a reversed phone cord swaps them).
#ifndef CONSOLE_SEL_ADC_A_PIN
#define CONSOLE_SEL_ADC_A_PIN 1  // J4 Select (the right knob), contact 2
#endif
#ifndef CONSOLE_SEL_ADC_B_PIN
#define CONSOLE_SEL_ADC_B_PIN 2  // contact 3
#endif
#ifndef CONSOLE_VOL_ADC_A_PIN
#define CONSOLE_VOL_ADC_A_PIN 4  // J5 Volume (the left knob), contact 2
#endif
#ifndef CONSOLE_VOL_ADC_B_PIN
#define CONSOLE_VOL_ADC_B_PIN 7  // contact 3
#endif

// J6: the 38 kHz IR receiver (a TSOP4838 on the board, pins 1 OUT, 2 GND, 3 VS on 3.3 V) for an IR remote. -1 = none.
#ifndef CONSOLE_IR_PIN
#define CONSOLE_IR_PIN 39
#endif
// An RGB LED (WS2812) that shows the IR receiver working: blue while it hears any IR, green when a known remote button
// decodes. 48 is the S3 DevKitC's own LED (38 on some revisions) and free on the console board. -1 = none.
#ifndef CONSOLE_IR_LED_PIN
#define CONSOLE_IR_LED_PIN 48
#endif

// Set to 1 when a knob counts the wrong way.
#ifndef CONSOLE_SEL_REVERSE
#define CONSOLE_SEL_REVERSE 0
#endif
#ifndef CONSOLE_VOL_REVERSE
#define CONSOLE_VOL_REVERSE 0
#endif
#ifndef CONSOLE_KNOB_STABLE_SAMPLES
#define CONSOLE_KNOB_STABLE_SAMPLES 3  // the knob lines are sampled at 1 kHz: a level counts once read this many times
#endif

#ifndef CONSOLE_HOLD_MS
#define CONSOLE_HOLD_MS 650  // a knob push or the right ring held this long = its hold action
#endif
