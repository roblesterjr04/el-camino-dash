#include "center_shell.h"
#include <elc_usb_link.h>

#include <esp_heap_caps.h>
#include <p4_screen.h>
#include <p4_touch.h>
#include <chan_screen.h>
#include <elc_fw_esp32.h>
#include <menu_can.h>
#include <phone_can.h>
#include <ui_menu.h>

#include "center_alerts.h"
#include "center_audio_link.h"
#include "center_camera.h"
#include "center_can.h"
#include "center_menu.h"
#include <elc_gauges.h>

#include <sensor_link.h>

#include "center_time.h"
#include "center_ui.h"
#include "display_role.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** "-6", "+5", "10": a whole-hour UTC offset rather than a POSIX string. */
static bool parse_hour_offset(const String &s, int *out) {
  const char *p = s.c_str();
  if (*p == '+' || *p == '-') {
    p++;
  }
  if (!*p || strlen(p) > 2) {
    return false;
  }
  for (const char *q = p; *q; q++) {
    if (!isdigit((unsigned char)*q)) {
      return false;
    }
  }
  const int h = atoi(s.c_str());
  if (h < -12 || h > 14) {
    return false;
  }
  *out = h;
  return true;
}

static void handle_shell_line(Preferences &prefs, const String &line) {
  if (line.length() == 0) {
    return;
  }
  if (display_role_command(line.c_str())) {  // role [left|center|right|auto]
    return;
  }
  {
    char reply[1024];
    if (phone_can_command(line.c_str(), reply, sizeof(reply)) ||  // phone, phone demo …
        menu_can_command(line.c_str(), reply, sizeof(reply)) ||   // menu, menu rotate|press|back|open … (not "menu on")
        chan_screen_command(line.c_str(), reply, sizeof(reply)) ||  // chan, chan query
        center_alerts_command(line.c_str(), reply, sizeof(reply)) ||  // alerts, alerts ack [all]
        elc_fw_esp32_command(line.c_str(), reply, sizeof(reply))) {  // fw, fw good
      Serial.println(reply);
      return;
    }
  }
  if (line == "shot" || line == "shot legacy") {
    // Screenshot of what the panel shows (or the legacy drawing layer); tools/screenshot.py drives this.
#if ARDUINO_USB_CDC_ON_BOOT
    p4_screen_shot(Serial, nullptr, 0, 0, line == "shot legacy");  // USB CDC: no baud to change
#else
    p4_screen_shot(Serial, &Serial, 2000000, 115200, line == "shot legacy");
#endif
    return;
  }
  if (line == "demo on" || line == "demo off") {
    center_set_demo(line == "demo on");
    Serial.println(line == "demo on" ? "ok: demo drive cycle on (sensor pack frames ignored)" : "ok: demo off (live sensor data)");
    return;
  }
  if (line == "knob left" || line == "knob right" || line == "knob press" || line == "knob back") {  // bench: encoder knob
    if (line == "knob press") {
      center_menu_press();
    } else if (line == "knob back") {
      ui_menu_back();
    } else {
      center_menu_rotate(line == "knob right" ? 1 : -1);
    }
    Serial.printf("ok: %s\n", line.c_str());
    return;
  }
  if (line.startsWith("menu on ") || line.startsWith("media on ")) {  // bench: "menu on left|center|right"
    const String w = line.substring(line.indexOf(" on ") + 4);
    const int scr = w == "left" ? 0 : (w == "center" ? 1 : (w == "right" ? 2 : -1));
    if (scr < 0) {
      Serial.println("usage: menu on left|center|right  /  media on left|center|right");
      return;
    }
    if (line.startsWith("menu")) {
      center_ui_set_interface_screen((uint8_t)scr);
    } else {
      center_ui_set_media_screen((uint8_t)scr);
    }
    Serial.printf("ok: menu on %d, media on %d (0 left, 1 center, 2 right)\n", (int)center_ui_interface_screen(),
                  (int)center_ui_media_screen());
    return;
  }
  if (line.startsWith("drag ")) {  // bench: "drag <x> <y1> <y2> <ms>"
    int x = 0, y1 = 0, y2 = 0;
    unsigned long ms = 400;
    if (sscanf(line.c_str() + 5, "%d %d %d %lu", &x, &y1, &y2, &ms) >= 3) {
      center_inject_drag((int16_t)x, (int16_t)y1, (int16_t)y2, (uint32_t)ms);
      Serial.printf("ok: drag %d,%d -> %d over %lu ms\n", x, y1, y2, ms);
    } else {
      Serial.println("err: drag <x> <y1> <y2> [ms]");
    }
    return;
  }
  if (line.startsWith("tap ")) {  // bench: simulate a touch, "tap <x> <y> [hold_ms]"
    int x = 0, y = 0;
    unsigned long hold = 80;
    if (sscanf(line.c_str() + 4, "%d %d %lu", &x, &y, &hold) >= 2) {
      center_inject_touch((int16_t)x, (int16_t)y, (uint32_t)hold);
      Serial.printf("ok: tap %d,%d for %lu ms\n", x, y, hold);
    } else {
      Serial.println("err: tap <x> <y> [hold_ms]");
    }
    return;
  }
  if (line.startsWith("theme ")) {  // bench: "theme 0|1|2" (Classic, Fallout, Modern)
    const int t = atoi(line.c_str() + 6);
    center_ui_set_theme(cluster_theme_from_reg((uint8_t)t));
    Serial.printf("ok: theme %d\n", (int)center_ui_cluster_theme());
    return;
  }
  if (line.startsWith("view ")) {  // bench: "view 0..3" (tach, clock, media, efficiency)
    const int v = atoi(line.c_str() + 5);
    if (v >= 0 && v <= 3) {
      center_ui_set_view((CenterViewMode)v);
      Serial.printf("ok: view %d\n", v);
    }
    return;
  }
  if (line == "cam" || line.startsWith("cam ")) {  // backup camera
    if (line == "cam on") center_camera_set_mode(CameraMode::On);
    if (line == "cam off") center_camera_set_mode(CameraMode::Off);
    if (line == "cam auto") center_camera_set_mode(CameraMode::Auto);
    if (line == "cam vsync") center_camera_toggle_vsync_invert();
    if (line == "cam pclk") center_camera_toggle_pclk_invert();
    if (line == "cam timing") {
      char timing[256];
      center_camera_field_timing(timing, sizeof(timing));
      Serial.printf("cam: %s\n", timing);
    }
    if (line == "cam bits") {
      static char bits[2048];
      center_camera_bit_check(bits, sizeof(bits));
      Serial.print(bits);
    }
    if (line == "cam pins") {
      char pins[200];
      center_camera_pin_edges(pins, sizeof(pins));
      Serial.printf("cam: %s\n", pins);
    }
    char st[512];
    center_camera_status(st, sizeof(st));
    Serial.printf("cam: %s\n", st);
    return;
  }
  if (line == "audio") {  // bench: what the audio bridge reports over CAN
    uint16_t el = 0, dur = 0;
    uint8_t vol = 0;
    const bool tr = center_audio_link_transport(&el, &dur, &vol);
    Serial.printf("audio: link=%d line=\"%s\" transport=%d elapsed=%us duration=%us vol=%u\n",
                  (int)center_audio_link_ok(), center_audio_link_line(), (int)tr, (unsigned)el, (unsigned)dur, (unsigned)vol);
    char art[160];
    center_audio_art_status(art, sizeof(art));
    Serial.printf("audio: art %s\n", art);
    return;
  }
  if (line == "can") {  // bench: CAN counters and the nodes heard (heartbeats)
    char st[320];
    center_can_describe(st, sizeof(st));
    Serial.println(st);
    return;
  }
  if (line == "perf") {
    const P4ScreenStats st = p4_screen_stats();
    const SensorPacketParser &sp = center_sensor_link().parser();
    Serial.printf("sensor: %lu frames/s, totals ok=%lu bad=%lu skipped=%lu bytes\n", (unsigned long)center_sensor_link().framesPerSec(),
                  (unsigned long)sp.framesOk(), (unsigned long)sp.framesBad(), (unsigned long)sp.bytesSkipped());
    const P4TouchStats ts = p4_touch_stats();
    Serial.printf("touch: %lu reads, %lu without news, %lu I2C errors, %lu brief lifts ignored\n", (unsigned long)ts.reads,
                  (unsigned long)ts.no_news, (unsigned long)ts.errors, (unsigned long)ts.glitches_ignored);
    Serial.printf("flip: wait %.1f ms, sync %.1f ms (%s, %.0f rows), %.1f areas per flip\n", (double)st.flip_wait_ms,
                  (double)st.flip_sync_ms, st.dma_sync ? "DMA" : "CPU", (double)st.sync_rows_per_flip, (double)st.areas_per_flip);
    Serial.printf("ui: view=%d theme=%d overlay=%d fullscreen=%d, last dial paint %lu ms\n", (int)center_ui_view_mode(),
                  (int)center_ui_cluster_theme(), (int)center_ui_overlay_open(), (int)center_ui_fullscreen(),
                  (unsigned long)gauges_last_paint_ms());
    Serial.printf("perf: %.1f fps, drawing %.1f ms + LVGL %.1f ms per frame, %.1f of 50 bands changed/frame, %lu rows/s written back, %s (%lu flips/s), heap %lu KB internal / %lu KB PSRAM free\n",
                  (double)st.fps, (double)st.app_ms, (double)st.lvgl_ms, (double)st.bands_changed, (unsigned long)st.flushed_rows,
                  st.double_buffered ? "double-buffered" : "single-buffered", (unsigned long)st.flips,
                  (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return;
  }
  if (line == "help") {
    Serial.println("Commands: shot [legacy] | perf | demo on|off | theme 0|1|2 | view 0..3 | cam [on|off|auto|vsync|pclk|pins|timing|bits] | audio | phone [demo idle|discover|incoming|active|waiting|held|pair|off] | menu [rotate <n>|press|back|open <0..6>] | can | alerts [ack [all]] | tap <x> <y> [ms] | knob left|right|press|back | role [left|center|right|auto] | menu on left|center|right | media on left|center|right | tz [<posix>|<hours>]");
    Serial.println("Time: UTC from GPS RMC/ZDA; local zone from tz (POSIX rules keep DST without a network).");
    return;
  }
  if (line == "tz" || line.startsWith("tz ")) {
    String rest = line.length() > 3 ? line.substring(3) : String();
    rest.trim();
    if (rest.length() == 0) {
      Serial.printf("tz: %s (set: tz <posix> e.g. CST6CDT,M3.2.0,M11.1.0, or tz <hours> e.g. -6, no DST)\n",
                    prefs.getString("tz", CENTER_TZ_DEFAULT).c_str());
      return;
    }
    int h = 0;
    if (parse_hour_offset(rest, &h)) {
      center_time_set_tz_utc_offset_hours(prefs, h);
    } else {
      center_time_set_tz_posix(prefs, rest.c_str());
    }
    Serial.printf("ok: tz %s\n", prefs.getString("tz", CENTER_TZ_DEFAULT).c_str());
    return;
  }
}

void center_shell_poll(Preferences &prefs) {
  static String acc;
  while (elc_usb_available()) {  // the USB link keeps CAN records out of the text
    char c = (char)elc_usb_read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      acc.trim();
      handle_shell_line(prefs, acc);
      acc.clear();
    } else if (acc.length() < 240) {
      acc += c;
    }
  }
}
