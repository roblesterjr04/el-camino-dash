#pragma once
// Strings and tables in flash on the AVR (8 KB of RAM would not hold the menu's text); plain memory on the host.
#if defined(ARDUINO_ARCH_AVR)
#include <avr/pgmspace.h>
#else
#include <string.h>
#define PROGMEM
#define PGM_P const char *
#define pgm_read_byte(p) (*(const uint8_t *)(p))
#define pgm_read_word(p) (*(const uint16_t *)(p))
#define pgm_read_dword(p) (*(const uint32_t *)(p))
#define pgm_read_ptr(p) (*(const void *const *)(p))
#define strncpy_P strncpy
#define strlen_P strlen
#define strcpy_P strcpy
#define memcpy_P memcpy
#define snprintf_P snprintf
#define PSTR(s) (s)
#endif
