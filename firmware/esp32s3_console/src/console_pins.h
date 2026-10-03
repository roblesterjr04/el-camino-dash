#pragma once

// Console board pins (ConsoleBoard4Key/README.md, "Pins") and input tuning. Every one can be overridden with -D.

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

// The four soft keys, top to bottom (to GND, 10k pull-ups).
#ifndef CONSOLE_KEY1_PIN
#define CONSOLE_KEY1_PIN 21
#endif
#ifndef CONSOLE_KEY2_PIN
#define CONSOLE_KEY2_PIN 47
#endif
#ifndef CONSOLE_KEY3_PIN
#define CONSOLE_KEY3_PIN 48
#endif
#ifndef CONSOLE_KEY4_PIN
#define CONSOLE_KEY4_PIN 13
#endif
// The knobs' RJ11 jacks: contacts 3 and 4 on ADC1, 33k pull-ups. Which contact carries the encoder and which the
// switches is found at run time (a reversed phone cord swaps them).
#ifndef CONSOLE_SEL_ADC_A_PIN
#define CONSOLE_SEL_ADC_A_PIN 1  // J4 Select (the right knob), contact 3
#endif
#ifndef CONSOLE_SEL_ADC_B_PIN
#define CONSOLE_SEL_ADC_B_PIN 2  // contact 4
#endif
#ifndef CONSOLE_VOL_ADC_A_PIN
#define CONSOLE_VOL_ADC_A_PIN 4  // J5 Volume (the left knob), contact 3
#endif
#ifndef CONSOLE_VOL_ADC_B_PIN
#define CONSOLE_VOL_ADC_B_PIN 7  // contact 4
#endif
#ifndef CONSOLE_LED_PIN
#define CONSOLE_LED_PIN 38  // all four key LEDs, through Q1
#endif

// Set to 1 when a knob counts the wrong way.
#ifndef CONSOLE_SEL_REVERSE
#define CONSOLE_SEL_REVERSE 0
#endif
#ifndef CONSOLE_VOL_REVERSE
#define CONSOLE_VOL_REVERSE 0
#endif
#ifndef CONSOLE_DEBOUNCE_MS
#define CONSOLE_DEBOUNCE_MS 20  // the keys
#endif
#ifndef CONSOLE_KNOB_STABLE_SAMPLES
#define CONSOLE_KNOB_STABLE_SAMPLES 3  // the knob lines are sampled at 1 kHz: a level counts once read this many times
#endif

#ifndef CONSOLE_HOLD_MS
#define CONSOLE_HOLD_MS 650  // any key or knob push held this long = its hold action
#endif
