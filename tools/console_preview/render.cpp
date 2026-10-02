// Host render of the console OLED: feeds the console's UI the CAN frames of a bench scenario and writes the
// 256×64 grey frame as a PGM (levels 0..15 scaled to 0..255). Usage: render <scenario> <theme 0|1|2> <out.pgm>
// Scenarios: media media-low radio hints presets trip engine clock menu menu-phone menu-display edit menu-radio source alert
// call-in call-active pair volume toast nophone waiting reverse boot

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <accessory_can.h>
#include <vector>

#include "console_ui.h"
#include "oled_gfx.h"

static ConsoleUi ui;
static uint32_t T = 10000;

static void feed(const ElcCanFrame &f) { ui.feed(f, T); }

// ---- the channel protocol: the console's node, and a simulated audio card (0x14) and radio (0x15)
static std::vector<ElcCanFrame> s_out;
static bool keep(const ElcCanFrame &f, void *ctx) {
  ((std::vector<ElcCanFrame> *)ctx)->push_back(f);
  return true;
}
static ElcChannels s_con, s_sink, s_radio;
static std::vector<ElcCanFrame> s_sink_out, s_radio_out;

static void deliver(std::vector<ElcCanFrame> &b) {
  for (const ElcCanFrame &f : b) s_con.feed(f, T);
  b.clear();
}

static void channels(bool radio_source) {
  ElcNetAnnounce me;
  me.type = ELC_TYPE_CONSOLE;
  s_con.begin(ELC_CAN_NODE_CONSOLE, me, keep, &s_out);
  s_con.onTransfer([](const ElcChTransfer &t, void *) { accessory_can_transfer(t, T); ui.transfer(t, T); }, nullptr);
  accessory_can_begin(&s_con);
  ui.attach(&s_con);
  ElcNetAnnounce a;
  a.type = ELC_TYPE_AUDIO_SINK;
  a.product = ELC_PRODUCT_AUDIO_CARD;
  snprintf(a.name, sizeof(a.name), "Audio (sim)");
  s_sink.begin(0x14, a, keep, &s_sink_out);
  s_sink.poll(0);
  ElcNetAnnounce r;
  r.type = ELC_TYPE_RADIO;
  r.product = ELC_PRODUCT_RADIO;
  r.produces = elc_ch_bit(ELC_CH_UI);
  snprintf(r.name, sizeof(r.name), "Radio (sim)");
  s_radio.begin(0x15, r, keep, &s_radio_out);
  s_radio.poll(0);
  ElcChWriter w;
  elc_audio_encode(ElcAudioStatus{60, 2, 0, 0, 15, radio_source ? ELC_SRC_RADIO : ELC_SRC_BLUETOOTH,
                                  (uint8_t)(radio_source ? 0x15 : 0x14)}, w);
  s_sink.send(ELC_CH_AUDIO_CMD, w);
  // the radio's page and what it plays
  ElcUiItem items[5];
  items[0].item = 0; items[0].kind = ELC_UI_TOGGLE; snprintf(items[0].label, 33, "Radio on");
  items[1].item = 1; items[1].kind = ELC_UI_CHOICE; snprintf(items[1].label, 33, "Band"); snprintf(items[1].options, 160, "FM\nAM");
  items[2].item = 2; items[2].kind = ELC_UI_NUMBER; items[2].exponent = -1; items[2].min = 875; items[2].max = 1080;
  items[2].step = 1; snprintf(items[2].label, 33, "Frequency"); snprintf(items[2].unit, 9, "MHz");
  items[3].item = 3; items[3].kind = ELC_UI_ACTION; snprintf(items[3].label, 33, "Seek up");
  items[4].item = 5; items[4].kind = ELC_UI_TEXTOUT; items[4].flags = ELC_UI_F_RO; snprintf(items[4].label, 33, "Station");
  for (auto &it : items) {
    ElcChWriter iw;
    elc_ui_encode(it, iw);
    s_radio.send(ELC_CH_UI, iw);
  }
  const ElcUiValue vals[3] = {{0, 1}, {1, 0}, {2, 947}};
  for (auto &v : vals) {
    ElcChWriter vw;
    elc_ui_encode(v, vw);
    s_radio.send(ELC_CH_UI, vw);
  }
  const uint32_t kPre[12] = {88500, 94700, 98300, 0, 101100, 1070, 0, 0, 104900, 0, 0, 0};
  for (int i = 0; i < 12; i++) {
    ElcUiItem pi;
    pi.item = (uint8_t)(15 + i);
    pi.parent = 14;
    pi.kind = ELC_UI_NUMBER;
    pi.flags = ELC_UI_F_ADVANCED | ELC_UI_F_SETTING;
    snprintf(pi.label, 33, "Preset %d", i + 1);
    ElcChWriter pw, vw;
    elc_ui_encode(pi, pw);
    elc_ui_encode(ElcUiValue{(uint8_t)(15 + i), (int32_t)kPre[i]}, vw);
    s_radio.send(ELC_CH_UI, pw);
    s_radio.send(ELC_CH_UI, vw);
  }
  ElcUiText txt;
  txt.item = 5;
  snprintf(txt.text, sizeof(txt.text), "THE WAVE");
  ElcChWriter tw;
  elc_ui_encode(txt, tw);
  s_radio.send(ELC_CH_UI, tw);
  if (radio_source) {
    ElcAudioNow np;
    np.source = ELC_SRC_RADIO;
    np.flags = ELC_NOW_F_STEREO | ELC_NOW_F_RDS;
    np.preset = 3;
    np.signal = 4;
    snprintf(np.line1, sizeof(np.line1), "94.7 FM  THE WAVE");
    snprintf(np.line2, sizeof(np.line2), "Now: Sea Breeze - Coastline");
    ElcChWriter nw;
    elc_audio_encode(np, nw);
    s_radio.send(ELC_CH_AUDIO_CMD, nw);
  }
  deliver(s_sink_out);
  deliver(s_radio_out);
}

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
  ElcCanHeartbeat hb;  // the console takes audio frames only from a node that says it is an audio bridge
  hb.node = ELC_CAN_NODE_SLOT0;
  hb.type = ElcCanNodeType::AudioBridge;
  ElcCanFrame h;
  elc_can_encode_heartbeat(hb, h);
  feed(h);
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
  ElcCanFrame f;
  elc_can_encode_time(t, f);
  feed(f);
}

/** Connected, no call: signal and battery for the media page's corner. */
static void phone_idle(uint8_t signal, uint8_t battery) {
  ElcCanPhoneStatus s;
  s.flags = (uint8_t)(ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP);
  s.signal = signal;
  s.battery = battery;
  ElcCanFrame f;
  elc_can_encode_phone_status(0, s, f);
  feed(f);
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
  T += ConsoleUi::kBootMs + 300;  // past the power-up bowtie
  center(theme);
  if (strcmp(sc, "waiting") != 0) {
    sensors(!strcmp(sc, "alert"));
    media(strcmp(sc, "nophone") != 0);
    clock_frames();
  }
  if (!strcmp(sc, "media")) phone_idle(4, 3);
  if (!strcmp(sc, "media-low")) phone_idle(1, 1);
  const bool radio_src = !strcmp(sc, "radio") || !strcmp(sc, "hints") || !strcmp(sc, "menu-radio") || !strcmp(sc, "presets") ||
                         !strcmp(sc, "menu") || !strcmp(sc, "source");
  if (radio_src || !strcmp(sc, "menu-phone")) channels(radio_src);
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
  if (!strcmp(sc, "trip")) { ui.soft_right(T); step(400); }
  if (!strcmp(sc, "engine")) { ui.soft_right(T); step(400); ui.soft_right(T); step(400); }
  if (!strcmp(sc, "clock")) { for (int i = 0; i < 3; i++) { ui.soft_right(T); step(400); } }
  if (!strcmp(sc, "hints")) { ui.right_turn(1, T); step(300); }
  if (!strcmp(sc, "presets")) { ui.right_click(T); ui.right_turn(2, T); step(200); }
  if (!strcmp(sc, "menu")) { ui.left_hold(T); step(100); }
  if (!strcmp(sc, "menu-display")) { ui.left_hold(T); ui.left_turn(3, T); ui.left_click(T); ui.left_turn(1, T); step(100); }
  if (!strcmp(sc, "edit")) {
    ui.left_hold(T); ui.left_turn(3, T); ui.left_click(T); ui.left_turn(1, T); ui.left_click(T); ui.left_turn(1, T); step(100);
  }
  if (!strcmp(sc, "menu-radio")) { ui.left_hold(T); ui.left_turn(1, T); ui.left_click(T); ui.left_turn(2, T); step(100); }
  if (!strcmp(sc, "menu-phone")) {
    ElcCanPhoneStatus ps;
    ps.flags = (uint8_t)(ELC_CAN_PHONE_CONNECTED | ELC_CAN_PHONE_HFP);
    ps.paired = 2;
    ps.list_serial = 2;
    ps.connected_index = 0;
    ElcCanFrame f, c[ELC_CAN_PHONE_TEXT_CHUNKS];
    elc_can_encode_phone_status(0, ps, f);
    feed(f);
    elc_can_encode_phone_text(0, ELC_CAN_PHONE_TEXT_CONNECTED, 0, "Rob's iPhone", c);
    for (auto &x : c) feed(x);
    elc_can_encode_phone_text(0, ELC_CAN_PHONE_TEXT_PAIRED + 0, 2, "Rob's iPhone", c);
    for (auto &x : c) feed(x);
    elc_can_encode_phone_text(0, ELC_CAN_PHONE_TEXT_PAIRED + 1, 2, "Rob's iPad", c);
    for (auto &x : c) feed(x);
    ui.left_hold(T); ui.left_turn(2, T); ui.left_click(T); ui.left_turn(3, T);
    feed(f);
  }
  if (!strcmp(sc, "source")) { ui.left_click(T); T += 300; }
  if (!strcmp(sc, "pair")) {
    ElcCanPhoneStatus ps;
    ps.flags = (uint8_t)(ELC_CAN_PHONE_PAIRING | ELC_CAN_PHONE_CONFIRM);
    ElcCanFrame f, c[ELC_CAN_PHONE_TEXT_CHUNKS];
    elc_can_encode_phone_status(0, ps, f);
    feed(f);
    elc_can_encode_phone_text(0, ELC_CAN_PHONE_TEXT_PAIRING, 0, "Pixel 8", c);
    for (auto &x : c) feed(x);
    elc_can_encode_phone_pairing(0, 482915, 110, f);
    feed(f);
  }
  if (!strcmp(sc, "call-in")) { phone(ELC_CAN_CALL_INCOMING, 0, 0); step(100); phone(ELC_CAN_CALL_INCOMING, 0, 0); }
  if (!strcmp(sc, "call-active")) { phone(ELC_CAN_CALL_ACTIVE, ELC_CAN_PHONE_CALL_AUDIO | ELC_CAN_PHONE_MIC_MUTED, 272); step(100); phone(ELC_CAN_CALL_ACTIVE, ELC_CAN_PHONE_CALL_AUDIO | ELC_CAN_PHONE_MIC_MUTED, 272); }
  if (!strcmp(sc, "volume")) { ui.left_turn(3, T); step(300); }
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
