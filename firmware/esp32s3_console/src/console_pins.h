#pragma once

// Console board pins (ConsoleBoard/README.md, "Pins") and input tuning. Every one can be overridden with -D.

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

#ifndef CONSOLE_BACK_PIN
#define CONSOLE_BACK_PIN 21  // left key
#endif
#ifndef CONSOLE_VIEW_PIN
#define CONSOLE_VIEW_PIN 47  // right key
#endif
#ifndef CONSOLE_SEL_A_PIN
#define CONSOLE_SEL_A_PIN 15
#endif
#ifndef CONSOLE_SEL_B_PIN
#define CONSOLE_SEL_B_PIN 16
#endif
#ifndef CONSOLE_SEL_SW_PIN
#define CONSOLE_SEL_SW_PIN 17
#endif
#ifndef CONSOLE_VOL_A_PIN
#define CONSOLE_VOL_A_PIN 18
#endif
#ifndef CONSOLE_VOL_B_PIN
#define CONSOLE_VOL_B_PIN 7
#endif
#ifndef CONSOLE_VOL_SW_PIN
#define CONSOLE_VOL_SW_PIN 14
#endif
#ifndef CONSOLE_LED_PIN
#define CONSOLE_LED_PIN 38  // all four key LEDs, through Q1
#endif

// Quadrature transitions per detent: 4 for most encoders (EC11 and the like), 2 for half-cycle ones, 1 for none.
#ifndef CONSOLE_ENC_STEPS_PER_DETENT
#define CONSOLE_ENC_STEPS_PER_DETENT 4
#endif
// Set to 1 when an encoder counts the wrong way (A / B swapped on the pigtail).
#ifndef CONSOLE_SEL_REVERSE
#define CONSOLE_SEL_REVERSE 0
#endif
#ifndef CONSOLE_VOL_REVERSE
#define CONSOLE_VOL_REVERSE 0
#endif
#ifndef CONSOLE_DEBOUNCE_MS
#define CONSOLE_DEBOUNCE_MS 20
#endif

#ifndef CONSOLE_HOLD_MS
#define CONSOLE_HOLD_MS 650  // Volume push held this long = next track (the prototype's hold)
#endif
