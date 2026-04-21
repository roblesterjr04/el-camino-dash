#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>

#include <boot_splash.h>
#include <cluster_i2c.h>
#include <display_setup.h>
#include <el_camino_speed.h>
#include <p4_overlay_menu.h>
#include <p4_touch.h>
#include <sensor_packet.h>

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

#ifndef SENSOR_PACKET_STALE_MS
#define SENSOR_PACKET_STALE_MS 400u
#endif

#ifndef WHEEL_CIRCUMFERENCE_M
#define WHEEL_CIRCUMFERENCE_M 2.1f
#endif
#ifndef SPEED_PULSES_PER_REV
#define SPEED_PULSES_PER_REV 1.0f
#endif

HardwareSerial &SensorSerial = Serial1;
static SensorPacketParser parser;
static Arduino_GFX *gfx = nullptr;
static Preferences s_prefs;
static P4OverlayMenu s_menu;

void setup() {
  Serial.begin(115200);
  delay(200);
  // Sensor UART before long display/touch init so packets are not missed during boot.
  SensorSerial.setRxBufferSize(2048);
  SensorSerial.begin(115200, SERIAL_8N1, SENSOR_UART_RX, SENSOR_UART_TX);
  parser.begin(SensorSerial);

  Serial.println("el-camino-dash CENTER / ESP32-P4 (WiFi+GPS time+BLE+I2C host UI)");

  gfx = create_tsps_display();
  if (!gfx) {
    Serial.println("FATAL: display init failed");
    while (true) {
      delay(1000);
    }
  }
  elc_boot_splash_framebuffer_write(gfx);
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
  center_gps_loop();
  center_shell_poll(s_prefs);
  center_wifi_ntp_loop();
  center_ble_loop();
  center_ui_loop();

  SensorPacketValues raw{};
  bool got = parser.poll(raw, true);
  {
    char uart_line[128];
    while (parser.takePendingTextLine(uart_line, sizeof(uart_line))) {
      center_ui_on_sensor_uart_line(uart_line);
    }
  }
  static uint32_t last_keepalive_ms = 0;
  uint32_t now = millis();
  if (now - last_keepalive_ms >= 350) {
    SensorSerial.write(PI_HANDSHAKE_BYTE);
    last_keepalive_ms = now;
  }

  static SensorPacketValues held{};
  static uint32_t last_pkt = 0;
  static bool have = false;
  if (got) {
    held = raw;
    last_pkt = now;
    have = true;
    center_ui_on_sensor_packet(raw);
  }

  SensorDerived derived{};
  if (have && (now - last_pkt) <= SENSOR_PACKET_STALE_MS) {
    derived = SensorPacketParser::derive(held);
  } else {
    derived.valid = false;
    derived.fuel_pct = 0.0f;
    derived.coolant_temp_f = 100.0f;
    derived.system_amp = 0.0f;
  }

  uint32_t rpm = 0;
  uint32_t speed_rpm = 0;
  if (have && (now - last_pkt) <= SENSOR_PACKET_STALE_MS) {
    rpm = held.engine_rpm;
    speed_rpm = held.speed_rpm;
  }

  int16_t tx = 0;
  int16_t ty = 0;
  bool finger_down = false;
  p4_touch_sample(&tx, &ty, &finger_down);

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

  const float mph = elc_speed_rpm_to_mph(speed_rpm, WHEEL_CIRCUMFERENCE_M, SPEED_PULSES_PER_REV);
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
  gin.fuel_pct = derived.valid ? derived.fuel_pct : 0.0f;
  gin.coolant_temp_f = derived.valid ? derived.coolant_temp_f : 100.0f;
  gin.system_amp = derived.valid ? derived.system_amp : 0.0f;
  gin.trip_mi = s_prefs.getFloat("trip_mi", 113.0f);
  gin.odometer_mi = s_prefs.getFloat("odo_mi", 45782.0f);
  gin.gear_num = gear;
  gin.mpg_average = 0.f;
  gin.mpg_instant = 0.f;
  gin.dte_tenths_mile = 0;
  gin.fuel_economy_valid = false;
  gin.current_street = center_gps_street_line();
  if (have && (now - last_pkt) <= SENSOR_PACKET_STALE_MS && held.has_fuel_economy) {
    gin.fuel_economy_valid = true;
    gin.mpg_average = (float)held.mpg_average_x10 * 0.1f;
    gin.mpg_instant = (float)held.mpg_instant_x10 * 0.1f;
    gin.dte_tenths_mile = held.dte_tenths_mile;
  }

  /* Poll AVRCP/transport immediately before paint so the cluster sees fresh I2C data. */
  center_audio_link_poll();

  if (!ui_block) {
    center_render(gfx, gfx->width(), gfx->height(), view, theme, &gin);
    if (theme == ClusterTheme::Classic && view != CenterViewMode::Media) {
      center_audio_link_draw(gfx, gfx->width(), gfx->height());
    }
  }
  const bool overlay_open = center_ui_overlay_open();
  center_media_lvgl_sync_overlay(overlay_open && (center_ui_view_mode() == CenterViewMode::Media));

  if (center_ui_fullscreen()) {
    center_ui_draw_fullscreen(gfx);
  } else if (overlay_open) {
    center_ui_draw_overlay();
  }
  /* DSI draws use auto_flush per primitive; flush(true) was forcing a full-frame cache msync every
   * loop (~66 Hz) and caused visible pulsing. flush(false) skips redundant full sync when auto_flush. */
  gfx->flush(false);

  SensorPacketValues post{};
  if (parser.poll(post, true)) {
    held = post;
    last_pkt = millis();
    have = true;
    center_ui_on_sensor_packet(post);
  }
  {
    char uart_line[128];
    while (parser.takePendingTextLine(uart_line, sizeof(uart_line))) {
      center_ui_on_sensor_uart_line(uart_line);
    }
  }

  delay(15);
}
