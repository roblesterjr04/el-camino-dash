// Host tests for SensorPacketParser (esp32_p4_common/src/sensor_packet.cpp).
// Frames are built exactly like sensor_pack.ino's sendPacket(). Run: ./run_tests.sh

#include <sensor_packet.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

struct Fields {
  uint16_t pot = 512, therm = 600, pres = 300, volt = 720, amb = 480;
  uint32_t speed_rpm = 350, engine_rpm = 2400;
  uint8_t flags = 0x03;
  uint16_t faults = 0;
  uint32_t odo = 966486;
  uint16_t mpg_i = 185, mpg_a = 172;
  uint32_t dte = 2750;
};

static void be16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back((uint8_t)(x >> 8));
  v.push_back((uint8_t)x);
}
static void be32(std::vector<uint8_t> &v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}

static std::vector<uint8_t> frame(const Fields &f) {
  std::vector<uint8_t> p;
  be16(p, f.pot);
  be16(p, f.therm);
  be16(p, f.pres);
  be16(p, f.volt);
  be16(p, f.amb);
  be32(p, f.speed_rpm);
  be32(p, f.engine_rpm);
  p.push_back(f.flags);
  be16(p, f.faults);
  be32(p, f.odo);
  be16(p, f.mpg_i);
  be16(p, f.mpg_a);
  be32(p, f.dte);
  uint8_t x = 0;
  for (uint8_t b : p) x ^= b;
  std::vector<uint8_t> out = {0xAA, 0x55};
  out.insert(out.end(), p.begin(), p.end());
  out.push_back(x);
  return out;
}

static std::vector<uint8_t> text(const char *s) {
  std::vector<uint8_t> v(s, s + std::strlen(s));
  v.push_back('\r');
  v.push_back('\n');
  return v;
}

static std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> out;
  for (auto &p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

/** Poll until no more frames; returns the frames parsed. */
static std::vector<SensorPacketValues> drain(SensorPacketParser &parser) {
  std::vector<SensorPacketValues> got;
  SensorPacketValues v{};
  while (parser.poll(v, false)) got.push_back(v);
  return got;
}

static std::vector<std::string> texts(SensorPacketParser &parser) {
  std::vector<std::string> out;
  char buf[128];
  while (parser.takePendingTextLine(buf, sizeof(buf))) out.emplace_back(buf);
  return out;
}

static void test_single_frame_fields() {
  std::printf("single frame decodes every field\n");
  HardwareSerial ser;
  SensorPacketParser parser;
  parser.begin(ser);
  Fields f;
  ser.feed(frame(f));
  auto got = drain(parser);
  CHECK(got.size() == 1);
  if (got.empty()) return;
  const auto &v = got[0];
  CHECK(v.valid && v.extended && v.has_odometer && v.has_fuel_economy);
  CHECK(v.pot_adc == f.pot && v.therm_adc == f.therm && v.pressure_adc == f.pres);
  CHECK(v.voltage_adc == f.volt && v.ambient_adc == f.amb);
  CHECK(v.speed_rpm == f.speed_rpm && v.engine_rpm == f.engine_rpm);
  CHECK(v.flags == f.flags && v.faults == f.faults);
  CHECK(v.odometer_tenths_mile == f.odo);
  CHECK(v.mpg_instant_x10 == f.mpg_i && v.mpg_average_x10 == f.mpg_a && v.dte_tenths_mile == f.dte);
}

static void test_partial_frame_waits() {
  // Regression: with a frame half-received, the old parser tried shorter legacy lengths and could
  // accept a bogus 19/21/25-byte "frame" (e.g. volt/ambient bytes read as speed).
  std::printf("partial frame is not parsed until complete (no legacy fallback)\n");
  HardwareSerial ser;
  SensorPacketParser parser;
  parser.begin(ser);
  auto fr = frame(Fields{});
  for (size_t cut : {3u, 21u, 23u, 27u, 35u}) {
    ser.feed(std::vector<uint8_t>(fr.begin(), fr.begin() + cut));
    CHECK(drain(parser).empty());
    ser.feed(std::vector<uint8_t>(fr.begin() + cut, fr.end()));
    auto got = drain(parser);
    CHECK(got.size() == 1 && got[0].speed_rpm == 350);
  }
}

static void test_text_between_frames_kept() {
  // Regression: text after a frame in the same read batch was silently dropped.
  std::printf("text lines before/between frames are queued, frames still parse\n");
  HardwareSerial ser;
  SensorPacketParser parser;
  parser.begin(ser);
  Fields a, b;
  b.engine_rpm = 3100;
  ser.feed(cat({text("######"), text("096648"), frame(a), text("FRAM rd v3 OK"), text("t=966486"), frame(b)}));
  auto got = drain(parser);
  CHECK(got.size() == 2);
  if (got.size() == 2) CHECK(got[1].engine_rpm == 3100);
  auto t = texts(parser);
  CHECK(t.size() == 4);
  if (t.size() == 4) {
    CHECK(t[0] == "######" && t[1] == "096648" && t[2] == "FRAM rd v3 OK" && t[3] == "t=966486");
  }
}

static void test_junk_then_frame_with_newline_byte() {
  // Regression: junk before a frame whose payload contains 0x0A made the text stripper eat the frame.
  std::printf("junk before a frame containing 0x0A does not swallow the frame\n");
  HardwareSerial ser;
  SensorPacketParser parser;
  parser.begin(ser);
  Fields f;
  f.therm = 0x000A;  // payload byte == '\n'
  ser.feed(cat({{0x13, 0x37, 0xFE}, frame(f)}));
  auto got = drain(parser);
  CHECK(got.size() == 1);
  if (!got.empty()) CHECK(got[0].therm_adc == 0x000A);
}

static void test_unknown_odometer() {
  std::printf("odometer 0xFFFFFFFF means unknown\n");
  HardwareSerial ser;
  SensorPacketParser parser;
  parser.begin(ser);
  Fields f;
  f.odo = 0xFFFFFFFFu;
  f.faults = 1u << 15;
  ser.feed(frame(f));
  auto got = drain(parser);
  CHECK(got.size() == 1);
  if (!got.empty()) CHECK(!got[0].has_odometer && got[0].odometer_tenths_mile == 0 && got[0].faults == 0x8000);
}

static void test_bad_checksum_resyncs() {
  std::printf("corrupt frame is rejected and the next good frame parses\n");
  HardwareSerial ser;
  SensorPacketParser parser;
  parser.begin(ser);
  auto bad = frame(Fields{});
  bad[10] ^= 0x40;
  Fields good;
  good.speed_rpm = 777;
  ser.feed(cat({bad, frame(good)}));
  auto got = drain(parser);
  CHECK(got.size() == 1);
  if (!got.empty()) CHECK(got[0].speed_rpm == 777);
}

static void test_byte_at_a_time() {
  std::printf("stream fed one byte at a time parses every frame\n");
  HardwareSerial ser;
  SensorPacketParser parser;
  parser.begin(ser);
  std::vector<uint8_t> stream;
  for (int i = 0; i < 20; i++) {
    Fields f;
    f.engine_rpm = 1000 + i;
    auto fr = frame(f);
    stream.insert(stream.end(), fr.begin(), fr.end());
    if (i % 5 == 0) {
      auto t = text("FRAM wr frac");
      stream.insert(stream.end(), t.begin(), t.end());
    }
  }
  std::vector<SensorPacketValues> got;
  for (uint8_t b : stream) {
    ser.feed({b});
    auto g = drain(parser);
    got.insert(got.end(), g.begin(), g.end());
  }
  CHECK(got.size() == 20);
  for (size_t i = 0; i < got.size(); i++) CHECK(got[i].engine_rpm == 1000 + i);
  CHECK(texts(parser).size() == 4);
}

static void test_handshake_only_when_asked() {
  std::printf("parser writes the keepalive only when asked\n");
  HardwareSerial ser;
  SensorPacketParser parser;
  parser.begin(ser);
  ser.feed(frame(Fields{}));
  SensorPacketValues v{};
  CHECK(parser.poll(v, false));
  CHECK(ser.tx.empty());
  ser.feed(frame(Fields{}));
  CHECK(parser.poll(v, true));
  CHECK(ser.tx.size() == 1 && ser.tx[0] == PI_HANDSHAKE_BYTE);
}

int main() {
  test_single_frame_fields();
  test_partial_frame_waits();
  test_text_between_frames_kept();
  test_junk_then_frame_with_newline_byte();
  test_unknown_odometer();
  test_bad_checksum_resyncs();
  test_byte_at_a_time();
  test_handshake_only_when_asked();
  if (g_failures) {
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall sensor_packet tests passed\n");
  return 0;
}
