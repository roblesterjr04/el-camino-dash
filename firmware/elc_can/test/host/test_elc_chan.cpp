// Host tests for the channel protocol (elc_can/src/elc_chan.cpp). Run: ./run_tests.sh

#include <elc_chan.h>

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_crc_and_frames() {
  std::printf("crc and framing\n");
  CHECK(elc_ch_crc16((const uint8_t *)"123456789", 9) == 0x29B1);  // CRC-16/CCITT-FALSE check value

  ElcCanFrame f[ELC_CH_MAX_FRAMES];
  const uint8_t two[2] = {0x01, 0x02};
  CHECK(elc_ch_frames(0x3D5, two, 2, 5, f, ELC_CH_MAX_FRAMES) == 1);
  CHECK(f[0].id == 0x3D5 && f[0].len == 3 && f[0].data[2] == 0xC5);  // SOT | EOT | tid 5

  // The same 40-byte transfer as tools/radio_sim.py makes (transfer ID 31): byte for byte.
  uint8_t p[40];
  for (int i = 0; i < 40; i++) p[i] = (uint8_t)i;
  const uint8_t want[6][8] = {{0x76, 0x04, 0x00, 0x01, 0x02, 0x03, 0x04, 0x9f}, {0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x3f},
                              {0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x1f}, {0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x3f},
                              {0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x1f}, {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x7f}};
  CHECK(elc_ch_frames(0x3D5, p, 40, 31, f, ELC_CH_MAX_FRAMES) == 6);
  for (int k = 0; k < 6; k++) CHECK(f[k].len == 8 && memcmp(f[k].data, want[k], 8) == 0);

  uint8_t big[257] = {0};
  CHECK(elc_ch_frames(0x3D5, big, 257, 0, f, ELC_CH_MAX_FRAMES) == 0);  // over 256
  CHECK(elc_ch_frames(0x3D5, big, 256, 0, f, ELC_CH_MAX_FRAMES) == 37);
  CHECK(elc_ch_frames(0x3D5, big, 0, 0, f, ELC_CH_MAX_FRAMES) == 0);

  CHECK(elc_ch_base(0x275) == ELC_CH_UI && elc_ch_base(0x3D5) == ELC_CH_NET && elc_ch_base(0x135) == ELC_CH_SENSOR);
  CHECK(elc_ch_base(0x100) == 0 && elc_ch_base(0x210) == 0 && elc_ch_base(0x305) == 0 && elc_ch_base(0x715) == 0);
}

static void test_reassembly() {
  std::printf("reassembly\n");
  uint8_t p[100];
  for (int i = 0; i < 100; i++) p[i] = (uint8_t)(i * 7);
  ElcCanFrame f[ELC_CH_MAX_FRAMES];
  const size_t n = elc_ch_frames(0x275, p, 100, 9, f, ELC_CH_MAX_FRAMES);
  CHECK(n == 15);

  ElcChRx rx;
  ElcChTransfer t;
  for (size_t i = 0; i + 1 < n; i++) CHECK(!rx.feed(f[i], 10, t));
  CHECK(rx.feed(f[n - 1], 10, t));
  CHECK(t.base == ELC_CH_UI && t.src == 0x15 && t.len == 100 && memcmp(t.data, p, 100) == 0);

  // A retried copy within 1 s is ignored; after 1 s it counts again.
  for (size_t i = 0; i + 1 < n; i++) rx.feed(f[i], 500, t);
  CHECK(!rx.feed(f[n - 1], 500, t));
  for (size_t i = 0; i + 1 < n; i++) rx.feed(f[i], 1600, t);
  CHECK(rx.feed(f[n - 1], 1600, t));

  // A lost frame (wrong toggle next), a gap over 100 ms, a bad CRC: all dropped.
  ElcChRx r2;
  r2.feed(f[0], 0, t);
  CHECK(!r2.feed(f[2], 0, t));
  for (size_t i = 3; i < n; i++) CHECK(!r2.feed(f[i], 0, t));
  ElcChRx r3;
  for (size_t i = 0; i + 1 < n; i++) r3.feed(f[i], i < 5 ? 0 : 200, t);
  CHECK(!r3.feed(f[n - 1], 200, t));
  ElcCanFrame bad[ELC_CH_MAX_FRAMES];
  memcpy(bad, f, sizeof(ElcCanFrame) * n);
  bad[3].data[2] ^= 0x10;
  ElcChRx r4;
  for (size_t i = 0; i + 1 < n; i++) r4.feed(bad[i], 0, t);
  CHECK(!r4.feed(bad[n - 1], 0, t));
  CHECK(r4.dropped() == 1);

  // Two senders interleaved on one channel reassemble apart (keyed by CAN ID).
  ElcCanFrame g[ELC_CH_MAX_FRAMES];
  const size_t m = elc_ch_frames(0x276, p, 60, 3, g, ELC_CH_MAX_FRAMES);
  ElcChRx r5;
  int done = 0;
  for (size_t i = 0; i < n || i < m; i++) {
    if (i < n && r5.feed(f[i], 0, t)) done += t.src == 0x15 && t.len == 100;
    if (i < m && r5.feed(g[i], 0, t)) done += t.src == 0x16 && t.len == 60;
  }
  CHECK(done == 2);
}

static void test_net_codecs() {
  std::printf("NET codecs\n");
  ElcNetAnnounce a;
  a.type = ELC_TYPE_RADIO;
  a.product = ELC_PRODUCT_RADIO;
  a.fw = 3;
  a.uid = 0x5D0A0009;
  a.generation = 2;
  a.produces = elc_ch_bit(ELC_CH_UI) | elc_ch_bit(ELC_CH_SENSOR);
  a.consumes = elc_ch_bit(ELC_CH_INPUT);
  a.flags = 1 | (2 << 1);
  std::strcpy(a.name, "Radio (sim)");
  uint8_t b[64];
  const size_t n = elc_net_encode_announce(a, b, sizeof(b));
  CHECK(n == 21 + 11);  // op, version, type, product, fw, uid, generation, produces, consumes, flags + name
  ElcNetAnnounce d;
  CHECK(elc_net_decode_announce(b, n, d));
  CHECK(d.type == 9 && d.product == 9 && d.uid == 0x5D0A0009 && d.generation == 2 && d.slot() == 2);
  CHECK(d.produces == a.produces && d.consumes == a.consumes && std::strcmp(d.name, "Radio (sim)") == 0);
  CHECK(!elc_net_decode_announce(b, 10, d));

  ElcNetRail r{0x14, 2, 0, 4800};
  CHECK(elc_net_encode_rail(r, b, sizeof(b)) == 6);
  ElcNetRail q;
  CHECK(elc_net_decode_rail(b, 6, q) && q.dest == 0x14 && q.slot == 2 && q.state == 0 && q.rate10 == 4800);
}

struct Bus {
  std::vector<ElcCanFrame> frames;
};
static bool bus_send(const ElcCanFrame &f, void *ctx) {
  ((Bus *)ctx)->frames.push_back(f);
  return true;
}
struct Seen {
  int transfers = 0;
  uint8_t described = 0;
  int identify = 0;
};
static void on_transfer(const ElcChTransfer &, void *ctx) { ((Seen *)ctx)->transfers++; }
static void on_describe(uint8_t what, void *ctx) { ((Seen *)ctx)->described |= what; }
static void on_net(uint8_t op, const uint8_t *, size_t, uint8_t, void *ctx) {
  if (op == ELC_NET_IDENTIFY) ((Seen *)ctx)->identify++;
}

static void test_service() {
  std::printf("channel service\n");
  Bus bus;
  Seen seen;
  ElcChannels ch;
  ElcNetAnnounce me;
  me.type = ELC_TYPE_DISPLAY;
  me.product = ELC_PRODUCT_DISPLAY;
  std::strcpy(me.name, "Center");
  ch.begin(0x01, me, bus_send, &bus);
  ch.onTransfer(on_transfer, &seen);
  ch.onDescribe(on_describe, &seen);
  ch.onNetCommand(on_net, &seen);

  ch.poll(0);  // boot: ANNOUNCE and descriptions
  CHECK(!bus.frames.empty() && bus.frames[0].id == 0x3C1);
  CHECK(seen.described == (ELC_NET_Q_ALL & ~ELC_NET_Q_ANNOUNCE));
  ElcChRx rx;
  ElcChTransfer t;
  bool got = false;
  for (const ElcCanFrame &f : bus.frames) got |= rx.feed(f, 0, t) && t.op() == ELC_NET_ANNOUNCE;
  ElcNetAnnounce back;
  CHECK(got && elc_net_decode_announce(t.data, t.len, back) && (back.produces & elc_ch_bit(ELC_CH_NET)));

  // Another node's ANNOUNCE lands in the node table.
  ElcChannels radio;
  Bus rbus;
  ElcNetAnnounce ra;
  ra.type = ELC_TYPE_RADIO;
  ra.product = ELC_PRODUCT_RADIO;
  ra.consumes = elc_ch_bit(ELC_CH_INPUT);
  std::strcpy(ra.name, "Radio");
  radio.begin(0x15, ra, bus_send, &rbus);
  radio.poll(0);
  for (const ElcCanFrame &f : rbus.frames) ch.feed(f, 5);
  CHECK(ch.nodeInfo(0x15).announced && ch.findNode(ELC_TYPE_RADIO) == 0x15 && ch.findConsumer(ELC_CH_INPUT) == 0x15);
  CHECK(seen.transfers >= 1);

  // QUERY to everyone: the radio answers after 0x15 x 8 = 168 ms, not before.
  rbus.frames.clear();
  ElcCanFrame q[ELC_CH_MAX_FRAMES];
  const uint8_t qp[3] = {ELC_NET_QUERY, 0x00, ELC_NET_Q_ANNOUNCE};
  CHECK(elc_ch_frames(0x3C1, qp, 3, 7, q, ELC_CH_MAX_FRAMES) == 1);
  radio.feed(q[0], 1000);
  radio.poll(1100);
  CHECK(rbus.frames.empty());
  radio.poll(1168);
  CHECK(rbus.frames.size() >= 1 && rbus.frames[0].id == 0x3D5);

  // Addressed to someone else: ignored. IDENTIFY to us: reported.
  const uint8_t idp[3] = {ELC_NET_IDENTIFY, 0x02, 5};
  elc_ch_frames(0x3D5, idp, 3, 1, q, ELC_CH_MAX_FRAMES);
  ch.feed(q[0], 2000);
  CHECK(seen.identify == 0);
  const uint8_t idq[3] = {ELC_NET_IDENTIFY, 0x01, 5};
  elc_ch_frames(0x3D5, idq, 3, 2, q, ELC_CH_MAX_FRAMES);
  ch.feed(q[0], 2001);
  CHECK(seen.identify == 1);

  // Our own frames (looped back by a relay) are not ours to receive.
  const int before = seen.transfers;
  for (const ElcCanFrame &f : bus.frames) ch.feed(f, 3000);
  CHECK(seen.transfers == before);

  // Transfer IDs advance per channel.
  bus.frames.clear();
  const uint8_t lp[3] = {0x01, 1, 'x'};
  ch.send(ELC_CH_LOG, lp, 3);
  ch.send(ELC_CH_LOG, lp, 3);
  CHECK(bus.frames.size() == 2 && (bus.frames[0].data[3] & 0x1F) + 1 == (bus.frames[1].data[3] & 0x1F));
}

int main() {
  test_crc_and_frames();
  test_reassembly();
  test_net_codecs();
  test_service();
  if (g_failures) {
    std::printf("%d FAILED\n", g_failures);
    return 1;
  }
  std::printf("all channel tests passed\n");
  return 0;
}
