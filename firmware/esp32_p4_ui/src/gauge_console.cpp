#include "gauge_console.h"
#include <elc_usb_link.h>

#include <esp_heap_caps.h>

#include "elc_gauges.h"
#include "p4_screen.h"

namespace {

void handle(GaugeConsole &c, SensorLink &link, String &line) {
  if (c.on_line && line.length() && c.on_line(line.c_str())) {
    return;
  }
  if (line == "shot") {
#if ARDUINO_USB_CDC_ON_BOOT
    p4_screen_shot(Serial, nullptr, 0, 0);  // USB CDC: no baud to change
#else
    p4_screen_shot(Serial, &Serial, 2000000, 115200);
#endif
  } else if (line == "demo on" || line == "demo off") {
    link.setDemo(line == "demo on");
    Serial.println(line == "demo on" ? "ok: demo drive cycle on" : "ok: demo off (live sensor data)");
  } else if (line.startsWith("theme ") && c.on_theme) {
    const int t = line.substring(6).toInt();
    c.on_theme((uint8_t)t);
    Serial.printf("ok: theme %d\n", t);
  } else if (line.startsWith("variant ") && c.on_variant) {
    c.on_variant((uint8_t)line.substring(8).toInt());
    Serial.println("ok: variant");
  } else if (line == "can" && c.on_can) {
    c.on_can();
  } else if (line == "perf") {
    const P4ScreenStats st = p4_screen_stats();
    const SensorPacketParser &sp = link.parser();
    Serial.printf("sensor: %lu frames/s, totals ok=%lu bad=%lu\n", (unsigned long)link.framesPerSec(),
                  (unsigned long)sp.framesOk(), (unsigned long)sp.framesBad());
    Serial.printf("ui: theme=%d face=%d, last dial paint %lu ms\n", (int)gauges_theme(), (int)gauges_face(),
                  (unsigned long)gauges_last_paint_ms());
    Serial.printf("perf: %.1f fps, LVGL %.1f ms per frame, flip wait %.1f ms + sync %.1f ms, heap %lu KB internal / %lu KB PSRAM free\n",
                  (double)st.fps, (double)st.lvgl_ms, (double)st.flip_wait_ms, (double)st.flip_sync_ms,
                  (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  } else if (line == "help") {
    Serial.println("Commands: shot | perf | demo on|off | theme 0|1|2 | variant 0|1 | can | phone [demo incoming|active|waiting|held|pair|off] | role [left|center|right|auto]");
  } else if (line.length()) {
    Serial.println("err: unknown command (help)");
  }
}

}  // namespace

void GaugeConsole::poll(SensorLink &link) {
  static String acc;
  while (elc_usb_available()) {  // the USB link keeps CAN records out of the text
    const char ch = (char)elc_usb_read();
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      acc.trim();
      handle(*this, link, acc);
      acc = "";
    } else if (acc.length() < 120) {
      acc += ch;
    }
  }
}
