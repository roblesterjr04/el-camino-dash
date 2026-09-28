#include <Arduino.h>
#include <cstring>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <esp_timer.h>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <display_setup.h>
#include <el_camino_speed.h>
#include <p4_overlay_menu.h>
#include <p4_screen.h>
#include <p4_touch.h>
#include <elc_watchdog.h>
#include <sensor_link.h>

#include "center_audio_link.h"
#include "center_ble.h"
#include "center_cluster.h"
#include "center_gps.h"
#include "center_media_ui.h"
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

void center_set_demo(bool on) { s_link.setDemo(on); }

void center_inject_touch(int16_t x, int16_t y, uint32_t hold_ms) {
  s_inject_x = x;
  s_inject_y = y;
  s_inject_until_ms = millis() + (hold_ms ? hold_ms : 1);
}
static Arduino_GFX *gfx = nullptr;
static Preferences s_prefs;
static P4OverlayMenu s_menu;

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
  // LVGL owns the panel from here; existing drawing code targets the legacy layer it returns.
  gfx = p4_screen_begin(panel);
  elc_illumination_gpio_poll();
  s_prefs.begin("center", false);
  center_sd_settings_begin();
  center_sd_settings_import(s_prefs);
  center_sd_settings_sync(s_prefs);

  p4_touch_begin();
  center_audio_link_begin();

  s_menu.begin(gfx);
  s_menu.setScreenSize(gfx->width(), gfx->height());
  s_menu.setBackdropColor(0x0000);

  center_wifi_ntp_begin(s_prefs);
  center_gps_begin(s_prefs);
  center_wifi_try_connect_saved();
  center_ble_begin();
  center_ble_set_info_line("ElCamino center");
  center_ui_begin(gfx, &s_prefs, &s_menu);

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

static uint32_t fnv(uint32_t h, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ p[i]) * 16777619u;
  }
  return h;
}

template <typename T>
static uint32_t fnv_v(uint32_t h, T v) {
  return fnv(h, &v, sizeof(v));
}

/** Everything the gauge views draw, quantized to what is visible. Equal signature → same pixels. */
static uint32_t gauge_signature(CenterViewMode view, ClusterTheme theme, const CenterGaugeInputs &g, bool ui_block) {
  uint32_t h = 2166136261u;
  h = fnv_v(h, (uint8_t)view);
  h = fnv_v(h, (uint8_t)theme);
  h = fnv_v(h, ui_block);
  h = fnv_v(h, g.sensor_valid);
  h = fnv_v(h, g.engine_rpm / 10u);
  h = fnv_v(h, (int32_t)(elc_speed_rpm_to_mph(g.speed_rpm, WHEEL_CIRCUMFERENCE_M, 1.0f) * 2.0f));
  h = fnv_v(h, (int32_t)(g.fuel_pct * 2.0f));
  h = fnv_v(h, (int32_t)g.coolant_temp_f);
  h = fnv_v(h, (int32_t)g.system_amp);
  h = fnv_v(h, g.odometer_valid);
  h = fnv_v(h, (int32_t)(g.odometer_mi * 10.0f));
  h = fnv_v(h, (int32_t)(g.trip_mi * 10.0f));
  h = fnv_v(h, g.gear_num);
  h = fnv_v(h, g.fuel_economy_valid);
  h = fnv_v(h, (int32_t)(g.mpg_average * 10.0f));
  h = fnv_v(h, (int32_t)(g.mpg_instant * 10.0f));
  h = fnv_v(h, g.dte_tenths_mile);
  h = fnv_v(h, g.clock_wall);
  h = fnv_v(h, g.clock_wall ? (uint32_t)(g.epoch_ms / 1000u) : g.millis_boot / 1000u);  // hands move per second
  if (g.current_street) {
    h = fnv(h, g.current_street, strlen(g.current_street));
  }
  const char *np = center_audio_link_line();
  if (np) {
    h = fnv(h, np, strlen(np));
  }
  h = fnv_v(h, center_audio_link_ok());
  uint16_t el = 0, dur = 0;
  uint8_t vol = 0;
  if (center_audio_link_transport(&el, &dur, &vol)) {
    h = fnv_v(h, el);
    h = fnv_v(h, dur);
  }
  if (view == CenterViewMode::Media) {
    h = fnv_v(h, millis() / 100u);  // progress needle interpolates between whole seconds
  }
  return h;
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

  const bool fresh = s_link.fresh();
  const SensorPacketValues &held = s_link.last();
  const SensorDerived derived = s_link.derived();
  const uint32_t rpm = fresh ? held.engine_rpm : 0;
  const uint32_t speed_rpm = fresh ? held.speed_rpm : 0;

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
    tx = s_inject_x;
    ty = s_inject_y;
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
  center_ui_touch_sample(tx, ty, finger_down);

  if (!finger_down && s_was_down) {
    const uint32_t dur = now - s_down_ms;
    clamp_touch(&s_lx, &s_ly, sw, sh);
    center_ui_process_touch_gesture(dur, s_lx, s_ly);
  }
  s_was_down = finger_down;

  uint64_t epoch_ms = 0;
  const bool wall_ok = center_wall_epoch_ms(&epoch_ms);
  const CenterViewMode view = center_ui_view_mode();
  const ClusterTheme theme = center_ui_cluster_theme();
  const bool clock_wall =
      (theme == ClusterTheme::Classic && (view == CenterViewMode::Clock || view == CenterViewMode::Media)) && wall_ok;

  static uint32_t frozen_rpm = 0;
  static uint32_t frozen_ms = 0;
  static bool ui_block_was = false;
  const bool ui_block = center_ui_overlay_open() || center_ui_fullscreen();
  if (ui_block && !ui_block_was) {
    frozen_rpm = rpm;
    frozen_ms = now;
  }
  ui_block_was = ui_block;
  const uint32_t render_rpm = ui_block ? frozen_rpm : rpm;
  const uint32_t render_ms = ui_block ? frozen_ms : now;

  const float mph = elc_speed_rpm_to_mph(speed_rpm, WHEEL_CIRCUMFERENCE_M, 1.0f);
  int gear = 2;
  if (mph < 4.0f) {
    gear = 1;
  } else if (mph > 55.0f || render_rpm > 4200u) {
    gear = 3;
  }

  CenterGaugeInputs gin{};
  gin.engine_rpm = render_rpm;
  gin.speed_rpm = speed_rpm;
  gin.millis_boot = render_ms;
  gin.epoch_ms = clock_wall ? epoch_ms : 0ull;
  gin.clock_wall = clock_wall;
  gin.sensor_valid = fresh;
  gin.fuel_pct = derived.fuel_pct;
  gin.coolant_temp_f = derived.coolant_temp_f;
  gin.system_amp = derived.system_amp;
  gin.odometer_valid = odo_ok;
  gin.odometer_mi = odometer_mi;
  gin.trip_mi = (odo_ok && s_trip_base_set) ? (odometer_mi - s_trip_base_mi) : 0.0f;
  gin.gear_num = gear;
  gin.mpg_average = 0.f;
  gin.mpg_instant = 0.f;
  gin.dte_tenths_mile = 0;
  gin.fuel_economy_valid = false;
  gin.current_street = center_gps_street_line();
  if (fresh && held.has_fuel_economy) {
    gin.fuel_economy_valid = true;
    gin.mpg_average = (float)held.mpg_average_x10 * 0.1f;
    gin.mpg_instant = (float)held.mpg_instant_x10 * 0.1f;
    gin.dte_tenths_mile = held.dte_tenths_mile;
  }

  /* Poll AVRCP/transport immediately before paint so the cluster sees fresh I2C data. */
  center_audio_link_poll();
  const int64_t draw_t0 = esp_timer_get_time();

  const bool overlay_open = center_ui_overlay_open();
  // The gauge renderers redraw the whole 480² buffer and rescale it (~125 ms), so only run them when
  // something they show has changed. Menus and full-screen pages still draw every frame.
  const uint32_t sig = gauge_signature(view, theme, gin, ui_block);
  static uint32_t s_last_sig = 0;
  static bool s_have_sig = false;
  const bool redraw = ui_block || !s_have_sig || sig != s_last_sig;
  s_last_sig = sig;
  s_have_sig = true;

  if (redraw) {
    if (!ui_block) {
      center_render(gfx, gfx->width(), gfx->height(), view, theme, &gin);
      if (theme == ClusterTheme::Classic && view != CenterViewMode::Media) {
        center_audio_link_draw(gfx, gfx->width(), gfx->height());
      }
    }
    center_media_lvgl_sync_overlay(overlay_open && (center_ui_view_mode() == CenterViewMode::Media));
    if (center_ui_fullscreen()) {
      center_ui_draw_fullscreen(gfx);
    } else if (overlay_open) {
      center_ui_draw_overlay();
    }
    // Legacy layer → LVGL → panel (LVGL redraws only the row bands that changed).
    p4_screen_note_app_us((uint32_t)(esp_timer_get_time() - draw_t0));
    p4_screen_legacy_dirty();
  }
  p4_screen_frame();

  poll_sensor_link();

  delay(15);
}
