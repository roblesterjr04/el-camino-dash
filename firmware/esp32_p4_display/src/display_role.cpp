#include "display_role.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#ifndef DISPLAY_ROLE_STRAP_GPIO
#define DISPLAY_ROLE_STRAP_GPIO 4
#endif

namespace {
const char *const kNs = "display";
const char *const kKey = "role";  // bench override only; the strap decides on the DigiDash board
uint8_t s_role = ELC_CAN_SCREEN_CENTER;
DisplayStrap s_strap = DisplayStrap::Open;
bool s_override = false;
}  // namespace

DisplayStrap display_role_read_strap() {
#if DISPLAY_ROLE_STRAP_GPIO >= 0
  const int pin = DISPLAY_ROLE_STRAP_GPIO;
  // Each pull ~45 kΩ against a 1 kΩ strap: a strapped pin stays put, an open pin follows the pull.
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(200);
  const int up = digitalRead(pin);
  pinMode(pin, INPUT_PULLDOWN);
  delayMicroseconds(200);
  const int down = digitalRead(pin);
  pinMode(pin, INPUT);  // no pull: no current through the strap afterwards
  if (!up && !down) return DisplayStrap::Low;
  if (up && down) return DisplayStrap::High;
  return DisplayStrap::Open;
#else
  return DisplayStrap::Open;
#endif
}

uint8_t display_role_resolve(DisplayStrap *strap_out, bool *override_out) {
  s_strap = display_role_read_strap();
  Preferences p;
  p.begin(kNs, true);
  const uint8_t ov = p.getUChar(kKey, 0xFF);
  p.end();
  s_override = ov <= ELC_CAN_SCREEN_RIGHT;
  if (s_override) {
    s_role = ov;
  } else {
    s_role = s_strap == DisplayStrap::Low ? ELC_CAN_SCREEN_LEFT
                                          : (s_strap == DisplayStrap::High ? ELC_CAN_SCREEN_RIGHT : ELC_CAN_SCREEN_CENTER);
  }
  if (strap_out) *strap_out = s_strap;
  if (override_out) *override_out = s_override;
  return s_role;
}

void display_role_set_override_and_restart(uint8_t role) {
  Preferences p;
  p.begin(kNs, false);
  if (role <= ELC_CAN_SCREEN_RIGHT) {
    p.putUChar(kKey, role);
  } else {
    p.remove(kKey);
  }
  p.end();
  Serial.printf("display: %s, restarting\n",
                role <= ELC_CAN_SCREEN_RIGHT ? display_role_name(role) : "override cleared (strap decides)");
  Serial.flush();
  delay(100);
  ESP.restart();
  while (true) {
  }
}

const char *display_role_name(uint8_t role) {
  switch (role) {
    case ELC_CAN_SCREEN_LEFT: return "left";
    case ELC_CAN_SCREEN_CENTER: return "center";
    case ELC_CAN_SCREEN_RIGHT: return "right";
    default: return "?";
  }
}

const char *display_strap_name(DisplayStrap s) {
  return s == DisplayStrap::Low ? "GND (left)" : (s == DisplayStrap::High ? "3V3 (right)" : "open (center)");
}

int display_role_parse(const char *s) {
  if (!s) return -1;
  while (*s == ' ') s++;
  if (!strcmp(s, "left") || !strcmp(s, "l") || !strcmp(s, "0")) return ELC_CAN_SCREEN_LEFT;
  if (!strcmp(s, "center") || !strcmp(s, "c") || !strcmp(s, "1")) return ELC_CAN_SCREEN_CENTER;
  if (!strcmp(s, "right") || !strcmp(s, "r") || !strcmp(s, "2")) return ELC_CAN_SCREEN_RIGHT;
  if (!strcmp(s, "auto")) return 0xFF;
  return -1;
}

uint8_t display_role_can_node(uint8_t role) {
  return role == ELC_CAN_SCREEN_LEFT ? ELC_CAN_NODE_SPEEDO : (role == ELC_CAN_SCREEN_RIGHT ? ELC_CAN_NODE_TSPS : ELC_CAN_NODE_CENTER);
}

bool display_role_command(const char *line) {
  if (strcmp(line, "role") == 0) {
    Serial.printf("role: %s screen (%s; strap on GPIO%d reads %s)\n", display_role_name(s_role),
                  s_override ? "bench override in NVS" : "from the strap", (int)DISPLAY_ROLE_STRAP_GPIO,
                  display_strap_name(s_strap));
    Serial.println("  role left|center|right  store a bench override and restart;  role auto  clear it");
    return true;
  }
  if (strncmp(line, "role ", 5) != 0) {
    return false;
  }
  const int r = display_role_parse(line + 5);
  if (r < 0) {
    Serial.println("usage: role [left|center|right|auto]");
    return true;
  }
  display_role_set_override_and_restart((uint8_t)r);
}
