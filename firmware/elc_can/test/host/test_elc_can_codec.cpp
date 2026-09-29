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
  ElcCanClusterState st;
  st.theme = 2;
  st.center_subview = 3;
  st.interface_screen = ELC_CAN_SCREEN_LEFT;
  st.media_screen = ELC_CAN_SCREEN_RIGHT;
  st.brightness = 60;
  st.flags = ELC_CAN_CLUSTER_MEDIA_SHOWN;
  elc_can_encode_cluster_state(st, f);
  CHECK(f.id == ELC_CAN_ID_CLUSTER_STATE && f.len == 6);
  ElcCanClusterState c;
  CHECK(elc_can_decode_cluster_state(f, c) && c.theme == 2 && c.center_subview == 3);
  CHECK(c.interface_screen == ELC_CAN_SCREEN_LEFT && c.media_screen == ELC_CAN_SCREEN_RIGHT);
  CHECK(c.brightness == 60 && c.flags == ELC_CAN_CLUSTER_MEDIA_SHOWN);
  // A center from before these settings: 2 bytes, defaults for the rest.
  f.len = 2;
  CHECK(elc_can_decode_cluster_state(f, c) && c.theme == 2 && c.interface_screen == ELC_CAN_SCREEN_CENTER);
  CHECK(c.media_screen == ELC_CAN_SCREEN_CENTER && c.brightness == 100 && (c.flags & ELC_CAN_CLUSTER_DIM_WITH_LIGHTS));
  // Out-of-range screens fall back to the center.
  f.len = 6;
  f.data[2] = 7;
  CHECK(elc_can_decode_cluster_state(f, c) && c.interface_screen == ELC_CAN_SCREEN_CENTER);
  f.len = 1;
  CHECK(!elc_can_decode_cluster_state(f, c));

  ElcCanSettingRequest rq{ELC_CAN_SET_MEDIA_SCREEN, ELC_CAN_SCREEN_LEFT, ELC_CAN_NODE_TSPS};
  elc_can_encode_setting_request(rq, f);
  ElcCanSettingRequest rr;
  CHECK(f.id == ELC_CAN_ID_SETTING_REQUEST && elc_can_decode_setting_request(f, rr));
  CHECK(rr.setting == ELC_CAN_SET_MEDIA_SCREEN && rr.value == ELC_CAN_SCREEN_LEFT && rr.from_node == ELC_CAN_NODE_TSPS);

  ElcCanCenterStatus cs;
  cs.flags = ELC_CAN_STATUS_WIFI | ELC_CAN_STATUS_GPS_FIX;
  cs.rssi = -61;
  cs.text_serial = 17;
  elc_can_encode_center_status(cs, f);
  ElcCanCenterStatus cr;
  CHECK(elc_can_decode_center_status(f, cr) && cr.flags == cs.flags && cr.rssi == -61 && cr.text_serial == 1);

  ElcCanFrame tf[ELC_CAN_TEXT_CHUNKS];
  elc_can_encode_text(ELC_CAN_ID_CENTER_TEXT, 4, "Garage", tf);
  ElcCanTextDecoder td(ELC_CAN_ID_CENTER_TEXT);
  bool done = false;
  for (size_t i = 0; i < ELC_CAN_TEXT_CHUNKS; i++) done = td.feed(tf[i]);
  CHECK(done && std::strcmp(td.text(), "Garage") == 0 && td.serial() == 4);
  ElcCanTextDecoder other(0x401);
  CHECK(!other.feed(tf[0]));

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
  CHECK(f.len == 8 && r.art_id == 0);
  t.art_id = 42;
  elc_can_encode_audio_transport(1, t, f);
  CHECK(elc_can_decode_audio_transport(f, &slot, r) && r.art_id == 42);
  f.len = 7;  // a bridge from before album art over CAN
  CHECK(elc_can_decode_audio_transport(f, &slot, r) && r.art_id == 0);
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

static void test_album_art() {
  std::printf("album art request, info and reassembly\n");
  ElcCanFrame f;
  elc_can_encode_art_request(ElcCanArtRequest{0, 7, 300, ELC_CAN_ART_JPEG128}, f);
  CHECK(f.id == ELC_CAN_ID_AUDIO_ART_REQUEST && f.len == 5);
  ElcCanArtRequest q;
  CHECK(elc_can_decode_art_request(f, q) && q.slot == 0 && q.art_id == 7 && q.first_chunk == 300);
  CHECK(q.variant == ELC_CAN_ART_JPEG128);
  f.len = 4;  // a requester from before variants: original
  CHECK(elc_can_decode_art_request(f, q) && q.variant == ELC_CAN_ART_ORIGINAL);

  // A 1003-byte "JPEG": 201 chunks, the last one 3 bytes.
  static uint8_t img[1003];
  for (size_t i = 0; i < sizeof(img); i++) img[i] = (uint8_t)(i * 7 + 3);
  const uint16_t n = elc_can_art_chunks(sizeof(img));
  CHECK(n == 201);
  static uint8_t buf[2048];
  ElcCanArtReceiver r;
  r.start(0, 7, ELC_CAN_ART_ORIGINAL, buf, sizeof(buf));
  CHECK(!r.sized() && !r.complete());

  // Data before the size is known is ours but not stored.
  ElcCanFrame d;
  elc_can_encode_art_data(0, 7, ELC_CAN_ART_ORIGINAL, 0, img, sizeof(img), d);
  CHECK(d.id == 0x403 && d.len == 8);
  CHECK(r.feed(d) && r.chunks_have() == 0);

  ElcCanFrame info;
  elc_can_encode_art_info(0, 7, ELC_CAN_ART_ORIGINAL, sizeof(img), info);
  CHECK(info.id == 0x402);
  CHECK(r.feed(info) && r.sized() && r.size() == sizeof(img));

  // Every other chunk, then the rest: first_missing walks the gaps.
  for (uint16_t i = 0; i < n; i += 2) {
    elc_can_encode_art_data(0, 7, ELC_CAN_ART_ORIGINAL, i, img, sizeof(img), d);
    CHECK(r.feed(d));
  }
  CHECK(!r.complete() && r.first_missing() == 1);
  elc_can_encode_art_data(0, 7, ELC_CAN_ART_ORIGINAL, 200, img, sizeof(img), d);
  CHECK(d.len == 3 + 3);
  // Foreign frames: another art id, another slot, the text message.
  ElcCanFrame other;
  elc_can_encode_art_data(0, 8, ELC_CAN_ART_ORIGINAL, 1, img, sizeof(img), other);
  CHECK(!r.feed(other));
  elc_can_encode_art_data(1, 7, ELC_CAN_ART_ORIGINAL, 1, img, sizeof(img), other);
  CHECK(!r.feed(other));
  for (uint16_t i = 1; i < n; i += 2) {
    elc_can_encode_art_data(0, 7, ELC_CAN_ART_ORIGINAL, i, img, sizeof(img), d);
    r.feed(d);
    r.feed(d);  // duplicates are harmless
  }
  CHECK(r.complete() && r.first_missing() == n && r.chunks_have() == n);
  CHECK(std::memcmp(buf, img, sizeof(img)) == 0);

  // An image bigger than the buffer is refused once, not written.
  ElcCanArtReceiver small;
  static uint8_t tiny[100];
  small.start(0, 7, ELC_CAN_ART_ORIGINAL, tiny, sizeof(tiny));
  CHECK(small.feed(info) && small.refused() && !small.sized());
  elc_can_encode_art_data(0, 7, ELC_CAN_ART_ORIGINAL, 0, img, sizeof(img), d);
  CHECK(!small.feed(d));

  // Two variants stream at once (center: 128 JPEG, console: 48 grey); each receiver takes only its own.
  static uint8_t gray[ELC_CAN_ART_GRAY48_BYTES], small_jpg[400];
  for (size_t i = 0; i < sizeof(gray); i++) gray[i] = (uint8_t)(i * 13);
  for (size_t i = 0; i < sizeof(small_jpg); i++) small_jpg[i] = (uint8_t)(i * 5 + 1);
  static uint8_t gbuf[ELC_CAN_ART_GRAY48_BYTES], jbuf[1024];
  ElcCanArtReceiver rg, rj;
  rg.start(0, 9, ELC_CAN_ART_GRAY48, gbuf, sizeof(gbuf));
  rj.start(0, 9, ELC_CAN_ART_JPEG128, jbuf, sizeof(jbuf));
  ElcCanFrame ig, ij;
  elc_can_encode_art_info(0, 9, ELC_CAN_ART_GRAY48, sizeof(gray), ig);
  elc_can_encode_art_info(0, 9, ELC_CAN_ART_JPEG128, sizeof(small_jpg), ij);
  CHECK(rg.feed(ig) && !rj.feed(ig) && rj.feed(ij) && !rg.feed(ij));
  const uint16_t ng = elc_can_art_chunks(sizeof(gray)), nj = elc_can_art_chunks(sizeof(small_jpg));
  for (uint16_t i = 0; i < (ng > nj ? ng : nj); i++) {
    ElcCanFrame a, b;
    if (i < ng) {
      elc_can_encode_art_data(0, 9, ELC_CAN_ART_GRAY48, i, gray, sizeof(gray), a);
      CHECK(!rj.feed(a));
      rg.feed(a);
    }
    if (i < nj) {
      elc_can_encode_art_data(0, 9, ELC_CAN_ART_JPEG128, i, small_jpg, sizeof(small_jpg), b);
      CHECK(!rg.feed(b));
      rj.feed(b);
    }
  }
  CHECK(rg.complete() && std::memcmp(gbuf, gray, sizeof(gray)) == 0);
  CHECK(rj.complete() && std::memcmp(jbuf, small_jpg, sizeof(small_jpg)) == 0);

  // "No such variant": size 0 is refused at once.
  ElcCanArtReceiver none;
  none.start(0, 9, ELC_CAN_ART_JPEG128, jbuf, sizeof(jbuf));
  elc_can_encode_art_info(0, 9, ELC_CAN_ART_JPEG128, 0, ij);
  CHECK(none.feed(ij) && none.refused());
}

static void test_inert_driver() {
  std::printf("driver without TWAI stays inert\n");
  ElcCan can;
  CHECK(!can.begin(34, 36, ELC_CAN_NODE_CENTER, ElcCanNodeType::Display));
  CHECK(!can.running());
  ElcCanFrame f;
  CHECK(!can.send(f));
  CHECK(can.txFree() == 0);
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
  test_album_art();
  test_inert_driver();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
