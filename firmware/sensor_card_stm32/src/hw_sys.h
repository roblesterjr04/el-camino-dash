#pragma once
#include <stdint.h>

// The chip itself: the clock tree (hw_sys.cpp replaces the variant's SystemClock_Config: 8 MHz HSE through the PLL to
// 64 MHz, HSI if the crystal does not start), the reset cause, the unique ID, the watchdog and restart.

/** True when the core runs from the crystal; false on the HSI fallback (the console still works, CAN timing may not). */
bool sys_clock_is_hse();
/** The reset flags, read at boot and printed by `status`: bit0 power-on / brown-out, bit1 NRST pin, bit2 option-byte
 *  loader, bit3 independent watchdog, bit4 window watchdog, bit5 software, bit6 low-power. */
uint8_t sys_reset_cause();
/** A 32-bit hash of the chip's 96-bit unique ID: never zero. */
uint32_t sys_unique_id();
void sys_watchdog_begin_2s();
void sys_watchdog_feed();
[[noreturn]] void sys_restart();
