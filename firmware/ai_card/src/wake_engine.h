#pragma once

// The wake word on the ESP32-S3 (Espressif esp-sr WakeNet). 16 kHz mono, 16-bit. One implementation per build:
// wake_engine.cpp (esp-sr), or none (AI_WAKE_ENGINE=0: press-to-talk only, which is how the card is brought up).

#include <stddef.h>
#include <stdint.h>

class WakeEngine {
 public:
  /** Loads the model from the "model" partition. False when there is none (the card then works with the key only). */
  bool begin();
  /** Samples per detect() call (WakeNet's chunk, typically 480 = 30 ms). 0 when not running. */
  size_t chunk() const { return _chunk; }
  /** Feed exactly chunk() samples. Returns the keyword number (1...) when the wake word was heard, else 0. */
  int detect(const int16_t *pcm);
  const char *name() const { return _name; }

 private:
  size_t _chunk = 0;
  char _name[32] = {0};
  void *_data = nullptr;
  const void *_iface = nullptr;
};
