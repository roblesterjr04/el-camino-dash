#include "theme_input.h"

#include <Arduino.h>
#include <Preferences.h>

#include <p4_touch.h>

#ifndef TSPS_THEME_GPIO
#define TSPS_THEME_GPIO -1
#endif

#ifndef TSPS_TOUCH_ENABLE
#define TSPS_TOUCH_ENABLE 1
#endif

#ifndef TSPS_TOUCH_SDA
#define TSPS_TOUCH_SDA 7
#endif

#ifndef TSPS_TOUCH_SCL
#define TSPS_TOUCH_SCL 8
#endif

static Preferences s_prefs;
static uint32_t s_last_gpio_ms;

void theme_input_begin() {
  s_prefs.begin("tsps", false);
#if TSPS_THEME_GPIO >= 0
  pinMode(TSPS_THEME_GPIO, INPUT_PULLUP);
#endif
#if TSPS_TOUCH_ENABLE
  p4_touch_begin(TSPS_TOUCH_SDA, TSPS_TOUCH_SCL);
#endif
}

bool theme_load_classic2() { return s_prefs.getUChar("c2", 1) != 0; }

void theme_save_classic2(bool classic2) { s_prefs.putUChar("c2", classic2 ? 1 : 0); }

void theme_poll_gpio_toggle(bool *toggled) {
  *toggled = false;
  uint32_t now = millis();

#if TSPS_THEME_GPIO >= 0
  static bool prev_gpio = HIGH;
  bool g = digitalRead(TSPS_THEME_GPIO);
  if (prev_gpio == HIGH && g == LOW && (now - s_last_gpio_ms) > 50u) {
    *toggled = true;
    s_last_gpio_ms = now;
  }
  prev_gpio = g;
#endif
}
