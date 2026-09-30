// Host render of the console OLED: feeds the console's UI the CAN frames of a bench scenario and writes the
// 256×64 grey frame as a PGM (levels 0..15 scaled to 0..255). Usage: render <scenario> <theme 0|1|2> <out.pgm>
// Scenarios: media trip engine clock menu edit alert call-in call-active remote volume toast nophone waiting reverse

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console_ui.h"
#include "oled_gfx.h"

static ConsoleUi ui;
static uint32_t T = 10000;

static void feed(const ElcCanFrame &f) { ui.feed(f, T); }

static void center(uint8_t theme) {
  ElcCanFrame f;
  f.id = ELC_CAN_ID_HEARTBEAT_BASE + ELC_CAN_NODE_CENTER;
  f.len = 8;
  feed(f);
  ElcCanClusterState c;
  c.theme = theme;
  c.brightness = 100;
  elc_can_encode_cluster_state(c, f);
  feed(f);
}

static void sensors(bool low_oil = false) {
  ElcCanSensor s;
  s.speed_rpm = 575;  // ~45 mph
  s.engine_rpm = 2200;
  s.pot_adc = 593;    // 58 % fuel
  s.pressure_adc = low_oil ? 50 : 409;
  s.therm_adc = 560;
  s.voltage_adc = 720;
  s.flags = 0x03;
  s.odometer_tenths_mile = 966486;
  s.dte_tenths_mile = 2750;
  s.mpg_instant_x10 = 185;
  s.mpg_average_x10 = 172;
  s.has_fuel_economy = true;
  ElcCanFrame f[ELC_CAN_SENSOR_FRAMES];
  elc_can_encode_sensor(s, f);
  for (auto &x : f) feed(x);
}

static void media(bool connected = true) {
  ElcCanAudioTransport t;
  t.elapsed_s = 95;
  t.duration_s = 270;
  t.volume = 9;
  t.connected = connected;
  t.text_serial = 1;
  ElcCanFrame f;
  elc_can_encode_audio_transport(0, t, f);
  feed(f);
  ElcCanFrame c[ELC_CAN_AUDIO_TEXT_CHUNKS];
  elc_can_encode_audio_text(0, 1, "Fleetwood Mac - The Chain (2004 Remaster)", c);
  for (auto &x : c) feed(x);
}

static void clock_frames() {
  ElcCanTime t;
  t.utc = 1790608980u;  // Mon Sep 28 2026, 15:23 UTC (10:23 local)
  t.offset_min = -300;
  t.flags = ELC_CAN_TIME_VALID | ELC_CAN_TIME_GPS;
  t.place_serial = 2;
  ElcCanFrame f;
  elc_can_encode_time(t, f);
  feed(f);
  ElcCanFrame c[ELC_CAN_TEXT_CHUNKS];
  elc_can_encode_text(ELC_CAN_ID_PLACE_TEXT, 2, "Liberty Street", c);
  for (auto &x : c) feed(x);
}

static void phone(uint8_t call, uint8_t flags, uint16_t secs) {
  ElcCanPhoneStatus s;
  s.flags = (uint8_t)(ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP | flags);
  s.call = call;
  s.call_s = secs;
  s.caller_serial = 4;
  s.call_volume = 11;
  ElcCanFrame f;
  elc_can_encode_phone_status(0, s, f);
  feed(f);
  ElcCanFrame c[ELC_CAN_PHONE_TEXT_CHUNKS];
  elc_can_encode_phone_text(0, ELC_CAN_PHONE_TEXT_CALLER, 4, "(555) 014-2368", c);
  for (auto &x : c) feed(x);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: render <scenario> <theme> <out.pgm>\n");
    return 1;
  }
  const char *sc = argv[1];
  const uint8_t theme = (uint8_t)atoi(argv[2]);
  if (!strcmp(sc, "boot")) {  // mid-way through the power-up sheen
    ui.tick(T);
    OledGfx g;
    ui.render(g, T + 1250);
    FILE *o = fopen(argv[3], "wb");
    fprintf(o, "P5\n256 64\n255\n");
    for (int i = 0; i < 256 * 64; i++) fputc(g.pixels()[i] * 17, o);
    fclose(o);
    return 0;
  }
  ui.tick(T);  // power-up
  T += 1500;
  center(theme);
  if (strcmp(sc, "waiting") != 0) {
    sensors(!strcmp(sc, "alert"));
    media(strcmp(sc, "nophone") != 0);
    clock_frames();
  }
  ui.tick(T);
  const auto step = [&](uint32_t ms) {
    T += ms;
    if (strcmp(sc, "waiting") != 0) {
      center(theme);
      sensors(!strcmp(sc, "alert"));
      media(strcmp(sc, "nophone") != 0);
    }
    ui.tick(T);
  };
  if (!strcmp(sc, "trip")) { ui.select_turn(1, T); step(400); }
  if (!strcmp(sc, "engine")) { ui.select_turn(1, T); step(400); ui.select_turn(1, T); step(400); }
  if (!strcmp(sc, "clock")) { ui.select_turn(-1, T); step(400); }
  if (!strcmp(sc, "menu")) { ui.select_press(T); ui.select_turn(2, T); step(100); }
  if (!strcmp(sc, "edit")) { ui.select_press(T); ui.select_turn(1, T); ui.select_press(T); ui.select_turn(1, T); step(100); }
  if (!strcmp(sc, "call-in")) { phone(ELC_CAN_CALL_INCOMING, 0, 0); step(100); phone(ELC_CAN_CALL_INCOMING, 0, 0); }
  if (!strcmp(sc, "call-active")) { phone(ELC_CAN_CALL_ACTIVE, ELC_CAN_PHONE_CALL_AUDIO | ELC_CAN_PHONE_MIC_MUTED, 272); step(100); phone(ELC_CAN_CALL_ACTIVE, ELC_CAN_PHONE_CALL_AUDIO | ELC_CAN_PHONE_MIC_MUTED, 272); }
  if (!strcmp(sc, "remote")) {
    ElcCanMenuState m;
    m.open = true;
    m.screen = ELC_CAN_SCREEN_CENTER;
    m.page = ELC_CAN_PAGE_HOME;
    m.opened_by = ELC_CAN_MENU_BY_TOUCH;
    m.text_serial = 3;
    ElcCanFrame f, c[ELC_CAN_TEXT_CHUNKS];
    elc_can_encode_menu_state(m, f);
    feed(f);
    elc_can_encode_text(ELC_CAN_ID_MENU_TEXT_MAIN, 3, "Phone", c);
    for (auto &x : c) feed(x);
    elc_can_encode_text(ELC_CAN_ID_MENU_TEXT_SUB, 3, "Connected \xC2\xB7 Rob's iPhone", c);
    for (auto &x : c) feed(x);
    step(100);
  }
  if (!strcmp(sc, "volume")) { ui.volume_turn(3, T); step(300); }
  if (!strcmp(sc, "toast")) {
    step(3000);
    ElcCanClusterState c;
    c.theme = theme;
    c.center_subview = 1;
    c.brightness = 100;
    ElcCanFrame f;
    elc_can_encode_cluster_state(c, f);
    feed(f);
    T += 400;
  }
  if (!strcmp(sc, "reverse")) {
    ElcCanCenterStatus s;
    s.flags = ELC_CAN_STATUS_REVERSE;
    ElcCanFrame f;
    elc_can_encode_center_status(s, f);
    feed(f);
  }
  if (!strcmp(sc, "waiting")) T += 4000;
  OledGfx g;
  ui.render(g, T);
  FILE *o = fopen(argv[3], "wb");
  fprintf(o, "P5\n256 64\n255\n");
  for (int i = 0; i < 256 * 64; i++) fputc(g.pixels()[i] * 17, o);
  fclose(o);
  return 0;
}
