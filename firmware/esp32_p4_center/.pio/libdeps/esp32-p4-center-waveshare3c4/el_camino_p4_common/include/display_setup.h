#pragma once

#include <stdint.h>

class Arduino_GFX;

// MIPI DSI: env esp32-p4-tsps = JD9165 720×720; esp32-p4-tsps-ev7 = stock 7" 1024×600 kit;
// esp32-p4-tsps-waveshare4c = Waveshare 720×720 JD9365 (PMIC 0x45 + init table).
Arduino_GFX *create_tsps_display();

// Backlight 0–255: LEDC on TSPS_BACKLIGHT_PIN (Waveshare 3.4C: GPIO 26) or PMIC 0x96 + optional SH8601 DCS when pin=-1.
// Not the same as ELC_ILLUMINATION_* — do not use one GPIO for both BL PWM and illum input.
void tsps_backlight_set_level(uint8_t level);

// Ambient illumination: ELC_ILLUMINATION_GPIO (default 24) INPUT_PULLUP → tsps_backlight_set_level. DIM_LEVEL default 100.
// Set -DELC_ILLUMINATION_GPIO=-1 to disable. -DELC_ILLUMINATION_LOW_IS_DIM=0 if harness inverts day/night sense.
// GPIO BL (e.g. 26): use -DTSPS_BACKLIGHT_ACTIVE_LOW=1 when FET is active-low. PMIC-only 3.4C: GFX patch for SH8601 DCS.
void elc_illumination_gpio_poll(void);
