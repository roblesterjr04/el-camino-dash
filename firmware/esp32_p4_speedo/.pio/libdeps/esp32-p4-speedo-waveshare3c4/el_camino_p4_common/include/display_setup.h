#pragma once

class Arduino_GFX;

// MIPI DSI: env esp32-p4-tsps = JD9165 720×720; esp32-p4-tsps-ev7 = stock 7" 1024×600 kit;
// esp32-p4-tsps-waveshare4c = Waveshare 720×720 JD9365 (PMIC 0x45 + init table).
Arduino_GFX *create_tsps_display();
