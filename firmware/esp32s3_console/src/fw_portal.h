#pragma once

// Firmware updates without USB (../README.md, Firmware updates): Menu → Service → Firmware update starts a Wi-Fi
// hotspot on the console with a password made up on the spot and shown on the OLED. A phone or laptop joins it, opens
// http://192.168.4.1 and uploads .elcfw files one at a time:
//   - the console's own file is written to its spare app slot, and the console restarts into it;
//   - a screen's file goes out over CAN as a BULK firmware WRITE (elc_fw.h) to every screen of that build at once (the
//     4" build reaches the left and right screens together), and each restarts into it when its copy checks out.
// The hotspot exists only while this page is open on the console: it closes on Back, after 10 minutes with nothing
// uploaded, and when the car moves. Updates start only while the car stands still. This is the one time the cluster
// uses Wi-Fi.

#include <stddef.h>
#include <stdint.h>

#include "oled_gfx.h"

class ElcCan;
class ElcChannels;

struct FwPortalConfig {
  ElcCan *can = nullptr;
  ElcChannels *chan = nullptr;
  /** The car stands still (no sensor data counts as standing). */
  bool (*stopped)() = nullptr;
  /** Draws the OLED while a long upload keeps the loop busy (the portal calls it about 10 times a second). */
  void (*redraw)(uint32_t now_ms) = nullptr;
};

void fw_portal_begin(const FwPortalConfig &cfg);
/** Opens the hotspot and the page. False (and a reason in fw_portal_status()) if the car is moving or Wi-Fi fails. */
bool fw_portal_start(uint32_t now_ms);
void fw_portal_stop();
bool fw_portal_active();
/** True while a file is being uploaded or sent to the screens (the console should not be left then). */
bool fw_portal_busy();
void fw_portal_poll(uint32_t now_ms);
/** The OLED page while the portal is open. */
void fw_portal_draw(OledGfx &g, uint32_t now_ms, const lv_font_t *title, const lv_font_t *body, const lv_font_t *small);
/** Serial console: `fw` (the update state and slots), `fw start`, `fw stop`, `fw good`. True if handled. */
bool fw_portal_command(const char *line, char *reply, size_t n);
