#pragma once

#include <stdint.h>

class Arduino_GFX;

// MIPI DSI: env esp32-p4-tsps = JD9165 720×720; esp32-p4-tsps-ev7 = stock 7" 1024×600 kit;
// esp32-p4-tsps-waveshare4c = Waveshare 720×720 JD9365 (PMIC 0x45 + init table).
Arduino_GFX *create_tsps_display();

// Backlight 0–255: Waveshare PMIC reg 0x96 when BL pin is disabled, else LEDC PWM or digital (≥128 on).
void tsps_backlight_set_level(uint8_t level);

// Ambient illumination input for any gauge (center / speedo / TSPS). Default GPIO 24 (labeled on 3.4C 40-pin
// header; GPIO 23 is not on that connector). Set -DELC_ILLUMINATION_GPIO=-1 to disable. INPUT_PULLUP: LOW → dim, HIGH → full.
void elc_illumination_gpio_poll(void);
