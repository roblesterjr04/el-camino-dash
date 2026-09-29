#include <Arduino.h>
#include <cstring>
#include <ctime>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <esp_timer.h>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <display_setup.h>
#include <elc_gauges.h>
#include <gauge_link.h>
#include <p4_screen.h>
#include <p4_touch.h>
#include <elc_watchdog.h>
#include <sensor_link.h>

#include "center_audio_link.h"
#include "center_camera.h"
#include "center_ble.h"
#include "center_menu.h"
#include "center_gps.h"
#include "center_sd_settings.h"
#include "center_ui.h"
#include "center_wifi_ntp.h"

SET_LOOP_TASK_STACK_SIZE(24 * 1024);

#ifndef SENSOR_UART_RX
#define SENSOR_UART_RX 5
#endif
#ifndef SENSOR_UART_TX
#define SENSOR_UART_TX 4
#endif

#ifndef WHEEL_CIRCUMFERENCE_M
#define WHEEL_CIRCUMFERENCE_M 2.1f
#endif

/** Re-send theme/sub-view to the other displays this often, so a board that booted late or
 *  browned out during cranking picks up the current state. */
#ifndef CLUSTER_REBROADCAST_MS
#define CLUSTER_REBROADCAST_MS 5000u
#endif

static SensorLink s_link;

// Console-injected touch (`tap x y ms`), used to drive the UI on the bench without a finger.
static int16_t s_inject_x = 0;
static int16_t s_inject_y = 0;
static uint32_t s_inject_until_ms = 0;
static uint32_t s_inject_start_ms = 0;
static int16_t s_inject_y2 = 0;  // drag end (== s_inject_y for a tap)

void center_set_demo(bool on) { s_link.setDemo(on); }

const SensorLink &center_sensor_link() { return s_link; }

void center_inject_touch(int16_t x, int16_t y, uint32_t hold_ms) { center_inject_drag(x, y, y, hold_ms); }

void center_inject_drag(int16_t x, int16_t y1, int16_t y2, uint32_t ms) {
  s_inject_x = x;
  s_inject_y = y1;
  s_inject_y2 = y2;
  s_inject_start_ms = millis();
  s_inject_until_ms = s_inject_start_ms + (ms ? ms : 1);
}
static Arduino_GFX *gfx = nullptr;
static Preferences s_prefs;

void setup() {
  Serial.begin(115200);
  delay(200);
  // Sensor UART before long display/touch init so packets are not missed during boot.
  s_link.begin(Serial1, SENSOR_UART_RX, SENSOR_UART_TX);

  Serial.println("el-camino-dash CENTER / ESP32-P4 (WiFi+GPS time+BLE+I2C host UI)");

  Arduino_GFX *panel = create_tsps_display();
  if (!panel) {
    Serial.println("FATAL: display init failed");
    while (true) {
      delay(1000);
    }
  }
  elc_boot_splash_framebuffer_write(panel);
  // LVGL owns the panel from here. Gauges are LVGL faces; the full-screen legacy pages (keyboard,
  // sensor log, GPS, phone) still draw into the legacy layer it returns.
  gfx = p4_screen_begin(panel);
  {
    const int w = gfx->width(), h = gfx->height(), size = w < h ? w : h;
    gauges_begin(p4_screen_gauge_parent(), (w - size) / 2, (h - size) / 2, size);
  }
  elc_illumination_gpio_poll();
  s_prefs.begin("center", false);
  center_sd_settings_begin();
  center_sd_settings_import(s_prefs);
  center_sd_settings_sync(s_prefs);

  p4_touch_begin();
  center_audio_link_begin();


  center_wifi_ntp_begin(s_prefs);
  center_gps_begin(s_prefs);
  center_wifi_try_connect_saved();
  center_ble_begin();
  center_ble_set_info_line("ElCamino center");
  center_ui_begin(gfx, &s_prefs);
  center_camera_begin();

  cluster_i2c_master_begin();
  center_ui_sync_cluster_bus();
  elc_loop_watchdog_begin();
}

static void poll_sensor_link() {
  if (s_link.poll()) {
    center_ui_on_sensor_packet(s_link.last());
  }
  char uart_line[128];
  while (s_link.takeTextLine(uart_line, sizeof(uart_line))) {
    center_ui_on_sensor_uart_line(uart_line);
  }
}

static void clamp_touch(int16_t *x, int16_t *y, int16_t w, int16_t h) {
  if (*x < 0) {
    *x = 0;
  }
  if (*y < 0) {
    *y = 0;
  }
  if (*x >= w) {
    *x = (int16_t)(w - 1);
  }
  if (*y >= h) {
    *y = (int16_t)(h - 1);
  }
}

static GaugeFace face_for(CenterViewMode v) {
  switch (v) {
    case CenterViewMode::Clock:
      return GaugeFace::Clock;
    case CenterViewMode::Media:
      return GaugeFace::Media;
    case CenterViewMode::Efficiency:
      return GaugeFace::Econ;
    case CenterViewMode::Tach:
    default:
      return GaugeFace::Tach;
  }
}

/** Wall time (GPS / NTP, local zone) or, without a source, time since boot. */
static void fill_clock(GaugeData &d, uint32_t now) {
  static const char *const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static char date[24];
  uint64_t epoch_ms = 0;
  if (center_wall_epoch_ms(&epoch_ms)) {
    const time_t t = (time_t)(epoch_ms / 1000u);
    struct tm lt;
    localtime_r(&t, &lt);
    d.time_valid = true;
    d.hour = lt.tm_hour;
    d.minute = lt.tm_min;
    d.second = (float)lt.tm_sec + (float)(epoch_ms % 1000u) / 1000.0f;
    snprintf(date, sizeof(date), "%s, %s %d", kDays[lt.tm_wday % 7], kMonths[lt.tm_mon % 12], lt.tm_mday);
    d.date_text = date;
  } else {
    const uint32_t s = now / 1000u;
    d.time_valid = false;
    d.hour = (int)((s / 3600u) % 24u);
    d.minute = (int)((s / 60u) % 60u);
    d.second = (float)(s % 60u) + (float)(now % 1000u) / 1000.0f;
    d.date_text = nullptr;
  }
  const char *street = center_gps_street_line();
  d.place_text = (street && street[0]) ? street : (d.time_valid ? "" : "Boot clock");
}

/** Now playing from the audio bridge ("Artist - Title"), with the position interpolated between reads. */
static void fill_media(GaugeData &d, uint32_t now) {
  static char artist[96];
  static char title[96];
  const char *line = center_audio_link_line();
  d.media_connected = center_audio_link_ok() && line && line[0];
  if (!d.media_connected) {
    return;
  }
  const char *sep = strstr(line, " - ");
  if (sep) {
    const size_t n = (size_t)(sep - line) < sizeof(artist) - 1 ? (size_t)(sep - line) : sizeof(artist) - 1;
    memcpy(artist, line, n);
    artist[n] = '\0';
    snprintf(title, sizeof(title), "%s", sep + 3);
  } else {
    artist[0] = '\0';
    snprintf(title, sizeof(title), "%s", line);
  }
  d.media_title = title;
  d.media_artist = artist;
  uint32_t art_id = 0;
  if (center_audio_art(&d.art_px, &d.art_w, &d.art_h, &d.art_stride, &art_id)) {
    d.art_id = art_id;
  }

  static uint16_t s_el = 0xFFFF;
  static uint32_t s_el_ms = 0;
  uint16_t el = 0, dur = 0;
  uint8_t vol = 0;
  if (center_audio_link_transport(&el, &dur, &vol) && dur > 0) {
    if (el != s_el) {
      s_el = el;
      s_el_ms = now;
    }
    const float since = (float)(now - s_el_ms) / 1000.0f;
    d.media_elapsed = (float)el + (since < 1.0f ? since : 1.0f);
    d.media_duration = (float)dur;
  }
}

void loop() {
  elc_illumination_gpio_poll();
  center_gps_loop();
  center_shell_poll(s_prefs);
  center_wifi_ntp_loop();
  center_ble_loop();
  center_ui_loop();

  poll_sensor_link();
  const uint32_t now = millis();

  static uint32_t s_last_rebroadcast_ms = 0;
  if (now - s_last_rebroadcast_ms >= CLUSTER_REBROADCAST_MS) {
    s_last_rebroadcast_ms = now;
    center_ui_sync_cluster_bus();
  }

  // Trip = miles since the first odometer reading this power cycle (sensor_pack is the source of truth).
  static bool s_trip_base_set = false;
  static float s_trip_base_mi = 0.0f;
  float odometer_mi = 0.0f;
  const bool odo_ok = s_link.odometerMiles(&odometer_mi);
  if (odo_ok && !s_trip_base_set) {
    s_trip_base_mi = odometer_mi;
    s_trip_base_set = true;
  }

  int16_t tx = 0;
  int16_t ty = 0;
  bool finger_down = false;
  p4_touch_sample(&tx, &ty, &finger_down);
  if (s_inject_until_ms && (int32_t)(now - s_inject_until_ms) < 0) {
    const uint32_t span = s_inject_until_ms - s_inject_start_ms;
    const float f = span ? (float)(now - s_inject_start_ms) / (float)span : 1.0f;
    tx = s_inject_x;
    ty = (int16_t)(s_inject_y + (s_inject_y2 - s_inject_y) * (f > 1.0f ? 1.0f : f));
    finger_down = true;
  } else {
    s_inject_until_ms = 0;
  }
  p4_screen_touch(tx, ty, finger_down);

  static bool s_was_down = false;
  static uint32_t s_down_ms = 0;
  static int16_t s_lx = 0;
  static int16_t s_ly = 0;

  const int16_t sw = gfx->width();
  const int16_t sh = gfx->height();

  if (finger_down && !s_was_down) {
    s_down_ms = now;
    s_lx = tx;
    s_ly = ty;
  }

  if (!finger_down && s_was_down) {
    const uint32_t dur = now - s_down_ms;
    clamp_touch(&s_lx, &s_ly, sw, sh);
    center_ui_process_touch_gesture(dur, s_lx, s_ly);
  }
  s_was_down = finger_down;
  // Progress ring while holding on the gauge screen (3 s = next view, 10 s = next theme).
  center_menu_hold_feedback(finger_down ? now - s_down_ms : 0,
                            finger_down && !center_menu_is_open() && !center_ui_fullscreen());

  /* Poll AVRCP/transport immediately before paint so the face sees fresh I2C data. */
  center_audio_link_poll();
  const int64_t draw_t0 = esp_timer_get_time();

  center_camera_loop(now);
  const bool fullscreen = center_ui_fullscreen();
  gauges_show((GaugeTheme)(uint8_t)center_ui_cluster_theme(), face_for(center_ui_view_mode()));
  gauges_set_hidden(fullscreen);
  p4_screen_set_legacy_visible(fullscreen);
  // While the menu covers the face, leave it be (the menu is what's moving).
  if (!fullscreen && !center_menu_is_open() && !center_camera_showing()) {
    GaugeData gd;
    gauge_fill_sensors(gd, s_link, WHEEL_CIRCUMFERENCE_M);
    gd.trip_mi = (gd.odo_valid && s_trip_base_set) ? (gd.odo_mi - s_trip_base_mi) : 0.0f;
    fill_clock(gd, now);
    fill_media(gd, now);
    gauges_update(gd, now);
  }
  if (fullscreen) {
    center_ui_draw_fullscreen(gfx);
    p4_screen_legacy_dirty();
  }
  p4_screen_note_app_us((uint32_t)(esp_timer_get_time() - draw_t0));
  p4_screen_frame();

  poll_sensor_link();

  // Pace to ~60 fps: sleep only for what is left of a 16 ms frame (a fixed delay(15) added 15 ms to
  // every frame, capping heavy frames far below that). Always yield at least 1 ms.
  static uint32_t s_frame_start_ms = 0;
  const uint32_t spent = millis() - s_frame_start_ms;
  delay(spent < 15 ? 16 - spent : 1);
  s_frame_start_ms = millis();
}
