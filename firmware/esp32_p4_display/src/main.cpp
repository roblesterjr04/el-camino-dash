#include <Arduino.h>
#include <elc_usb_link.h>

#include "display_main.h"
#include "display_role.h"

// One firmware for the three round displays: the position strap on header pin 17 (display_role.h) says whether this
// board is the left, center or right screen, read before anything else touches that pin.

SET_LOOP_TASK_STACK_SIZE(24 * 1024);

static bool s_center = true;

void setup() {
  DisplayStrap strap = DisplayStrap::Open;
  bool override = false;
  const uint8_t role = display_role_resolve(&strap, &override);
  elc_usb_begin();  // Serial at 115200, with the USB CAN link (docs/protocols/usb_link.md)
  delay(200);
  Serial.printf("display: %s screen (%s; strap reads %s)\n", display_role_name(role),
                override ? "bench override from `role`" : "from the strap", display_strap_name(strap));
  s_center = role == ELC_CAN_SCREEN_CENTER;
  if (s_center) {
    center_setup();
  } else {
    side_setup(role);
  }
}

void loop() {
  if (s_center) {
    center_loop();
  } else {
    side_loop();
  }
}
