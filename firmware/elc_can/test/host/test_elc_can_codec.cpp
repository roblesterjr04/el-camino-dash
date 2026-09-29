// Host tests for the CAN frame codecs (elc_can/src/elc_can_codec.cpp). Run: ./run_tests.sh

#include <elc_can.h>
#include <elc_can_codec.h>

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_sensor_round_trip() {
  std::printf("sensor round trip\n");
  ElcCanSensor s;
  s.pot_adc = 512;
  s.therm_adc = 600;
  s.pressure_adc = 300;
  s.voltage_adc = 720;
  s.ambient_adc = 480;
  s.speed_rpm = 70000;  // > 16 bits on purpose
  s.engine_rpm = 2400;
  s.flags = 0x03;
  s.faults = 0x8201;
  s.odometer_tenths_mile = 966486;
  s.dte_tenths_mile = 2750;
  s.mpg_instant_x10 = 185;
  s.mpg_average_x10 = 172;
  s.has_fuel_economy = true;
  s.seq = 42;

  ElcCanFrame f[ELC_CAN_SENSOR_FRAMES];
  elc_can_encode_sensor(s, f);
  CHECK(f[0].id == 0x100 && f[1].id == 0x101 && f[2].id == 0x102 && f[3].id == 0x103 && f[4].id == 0x104);
  for (const ElcCanFrame &x : f) CHECK(x.len >= 1 && x.len <= 8);
  // Little-endian on the wire.
  CHECK(f[1].data[0] == 0x00 && f[1].data[1] == 0x02);

  ElcCanSensorDecoder d;
  for (size_t i = 0; i + 1 < ELC_CAN_SENSOR_FRAMES; i++) {
    CHECK(d.feed(f[i]));
    CHECK(!d.complete());
  }
  CHECK(d.feed(f[ELC_CAN_SENSOR_FRAMES - 1]));
  CHECK(d.complete());
  const ElcCanSensor &v = d.value();
  CHECK(v.pot_adc == 512 && v.therm_adc == 600 && v.pressure_adc == 300 && v.voltage_adc == 720);
  CHECK(v.ambient_adc == 480 && v.flags == 0x03 && v.faults == 0x8201 && v.seq == 42);
  CHECK(v.speed_rpm == 70000 && v.engine_rpm == 2400);
  CHECK(v.odometer_tenths_mile == 966486 && v.dte_tenths_mile == 2750);
  CHECK(v.mpg_instant_x10 == 185 && v.mpg_average_x10 == 172 && v.has_fuel_economy);

  // Unknown odometer survives the trip.
  s.odometer_tenths_mile = 0xFFFFFFFFu;
  elc_can_encode_sensor(s, f);
  CHECK(d.feed(f[3]) && d.value().odometer_tenths_mile == 0xFFFFFFFFu);

  // Short or foreign frames are rejected.
  ElcCanFrame shortf = f[0];
  shortf.len = 4;
  ElcCanSensorDecoder d2;
  CHECK(!d2.feed(shortf));
  ElcCanFrame other;
  other.id = 0x210;
  other.len = 8;
  CHECK(!d2.feed(other));
}

static void test_cluster_and_heartbeat() {
  std::printf("cluster state and heartbeat\n");
  ElcCanFrame f;
  elc_can_encode_cluster_state(ElcCanClusterState{2, 3}, f);
  CHECK(f.id == ELC_CAN_ID_CLUSTER_STATE && f.len == 2);
  ElcCanClusterState c;
  CHECK(elc_can_decode_cluster_state(f, c) && c.theme == 2 && c.center_subview == 3);
  f.len = 1;
  CHECK(!elc_can_decode_cluster_state(f, c));

  ElcCanHeartbeat h;
  h.node = ELC_CAN_NODE_SLOT0 + 2;
  h.type = ElcCanNodeType::AudioBridge;
  h.fw_version = 0x0102;
  h.uptime_s = 123456;
  h.flags = 0x80;
  elc_can_encode_heartbeat(h, f);
  CHECK(f.id == 0x712 && f.len == 8);
  ElcCanHeartbeat r;
  CHECK(elc_can_decode_heartbeat(f, r));
  CHECK(r.node == 0x12 && r.type == ElcCanNodeType::AudioBridge && r.fw_version == 0x0102);
  CHECK(r.uptime_s == 123456 && r.flags == 0x80);
  f.id = 0x6FF;
  CHECK(!elc_can_decode_heartbeat(f, r));
}

static void test_slot_ranges() {
  std::printf("slot ranges\n");
  CHECK(elc_can_slot_id(0, 0) == 0x400 && elc_can_slot_id(1, 1) == 0x441 && elc_can_slot_id(2, 0x3F) == 0x4BF);
  CHECK(elc_can_slot_of(0x3FF) == -1 && elc_can_slot_of(0x400) == 0 && elc_can_slot_of(0x47F) == 1);
  CHECK(elc_can_slot_of(0x4BF) == 2 && elc_can_slot_of(0x4C0) == -1);
}

static void test_audio() {
  std::printf("audio transport and text\n");
  ElcCanAudioTransport t;
  t.elapsed_s = 95;
  t.duration_s = 241;
  t.volume = 11;
  t.text_serial = 0x1F;  // only the low 4 bits travel
  t.connected = true;
  ElcCanFrame f;
  elc_can_encode_audio_transport(1, t, f);
  CHECK(f.id == 0x440);
  uint8_t slot = 9;
  ElcCanAudioTransport r;
  CHECK(elc_can_decode_audio_transport(f, &slot, r));
  CHECK(slot == 1 && r.elapsed_s == 95 && r.duration_s == 241 && r.volume == 11 && r.text_serial == 0x0F && r.connected);
  ElcCanFrame textf[ELC_CAN_AUDIO_TEXT_CHUNKS];
  elc_can_encode_audio_text(1, 3, "x", textf);
  CHECK(!elc_can_decode_audio_transport(textf[0], &slot, r));

  const char *song = "The Beach Boys - Little Deuce Coupe (Mono)";  // longer than 32: cut
  elc_can_encode_audio_text(2, 5, song, textf);
  ElcCanAudioTextDecoder d;
  CHECK(!d.valid());
  // Out of order is fine; completes on the last missing chunk.
  CHECK(!d.feed(textf[4]));
  CHECK(!d.feed(textf[0]));
  CHECK(!d.feed(textf[2]));
  CHECK(!d.feed(textf[1]));
  CHECK(d.feed(textf[3]));
  CHECK(d.valid() && d.serial() == 5);
  CHECK(std::strlen(d.text()) == ELC_CAN_AUDIO_TEXT_MAX);
  CHECK(std::strncmp(d.text(), song, ELC_CAN_AUDIO_TEXT_MAX) == 0);

  // A new serial replaces the text; stale chunks from the old one are dropped.
  ElcCanFrame next[ELC_CAN_AUDIO_TEXT_CHUNKS];
  elc_can_encode_audio_text(2, 6, "Dick Dale - Misirlou", next);
  CHECK(!d.feed(next[0]));
  CHECK(!d.feed(textf[1]));  // old serial restarts collection for serial 5 ...
  for (size_t i = 0; i < ELC_CAN_AUDIO_TEXT_CHUNKS; i++) d.feed(next[i]);  // ... then 6 arrives whole
  CHECK(std::strcmp(d.text(), "Dick Dale - Misirlou") == 0 && d.serial() == 6);

  // Empty text (phone disconnected) is a valid, empty result.
  elc_can_encode_audio_text(0, 7, "", next);
  for (size_t i = 0; i < ELC_CAN_AUDIO_TEXT_CHUNKS; i++) d.feed(next[i]);
  CHECK(d.text()[0] == '\0' && d.serial() == 7);
}

static void test_inert_driver() {
  std::printf("driver without TWAI stays inert\n");
  ElcCan can;
  CHECK(!can.begin(34, 36, ELC_CAN_NODE_CENTER, ElcCanNodeType::Display));
  CHECK(!can.running());
  ElcCanFrame f;
  CHECK(!can.send(f));
  can.poll();
  char line[160];
  can.describe(line, sizeof(line));
  CHECK(std::strstr(line, "can: off node 0x01") == line);
  CHECK(!can.nodeAlive(ELC_CAN_NODE_SPEEDO));
}

int main() {
  test_sensor_round_trip();
  test_cluster_and_heartbeat();
  test_slot_ranges();
  test_audio();
  test_inert_driver();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
