#pragma once

class Arduino_GFX;

// Returns nullptr if not ESP32-P4 or display init fails.
Arduino_GFX *create_litmus_display();
