// Host tests for the channel protocol (elc_can/src/elc_chan.cpp). Run: ./run_tests.sh

#include <elc_chan.h>
#include <accessory_can.h>
#include <elc_chan_msg.h>

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

  // A channel node's heartbeat from a node not announced yet: one QUERY to it, not again within 5 s.
  bus.frames.clear();
  ElcCanFrame hb;
  hb.id = 0x714;
  hb.len = 8;
  hb.data[7] = 1;
  ch.feed(hb, 4000);
  ch.poll(4001);
  CHECK(bus.frames.size() == 1 && bus.frames[0].id == 0x3C1 && bus.frames[0].data[0] == ELC_NET_QUERY &&
        bus.frames[0].data[1] == 0x14 && bus.frames[0].data[2] == ELC_NET_Q_ALL);
  ch.feed(hb, 4500);
  ch.poll(4501);
  CHECK(bus.frames.size() == 1);
  hb.data[7] = 0;  // an old node (not on the channel protocol): never asked
  hb.id = 0x712;
  ch.feed(hb, 20000);
  ch.poll(20001);
  CHECK(bus.frames.size() == 1);

  // Transfer IDs advance per channel.
  bus.frames.clear();
  const uint8_t lp[3] = {0x01, 1, 'x'};
  ch.send(ELC_CH_LOG, lp, 3);
  ch.send(ELC_CH_LOG, lp, 3);
  CHECK(bus.frames.size() == 2 && (bus.frames[0].data[3] & 0x1F) + 1 == (bus.frames[1].data[3] & 0x1F));
}

static ElcChTransfer as_transfer(uint16_t base, uint8_t src, const ElcChWriter &w) {
  ElcChTransfer t;
  t.base = base;
  t.src = src;
  t.data = w.data();
  t.len = w.size();
  return t;
}

static void test_messages() {
  std::printf("channel messages\n");
  {  // UI ITEM bytes exactly as tools/radio_sim.py sends them
    static const uint8_t freq[] = {0x01, 0x02, 0xff, 0x03, 0x10, 0xff, 0x6b, 0x03, 0x00, 0x00, 0x38, 0x04, 0x00, 0x00, 0x01, 0x00,
                                   0x00, 0x00, 0x46, 0x72, 0x65, 0x71, 0x75, 0x65, 0x6e, 0x63, 0x79, 0x00, 0x4d, 0x48, 0x7a};
    ElcChTransfer t;
    t.base = ELC_CH_UI;
    t.src = 0x15;
    t.data = freq;
    t.len = sizeof(freq);
    ElcUiItem it;
    CHECK(elc_ui_decode(t, it));
    CHECK(it.item == 2 && it.parent == ELC_UI_TOP && it.kind == ELC_UI_NUMBER && it.flags == ELC_UI_F_SETTING);
    CHECK(it.exponent == -1 && it.min == 875 && it.max == 1080 && it.step == 1);
    CHECK(std::strcmp(it.label, "Frequency") == 0 && std::strcmp(it.unit, "MHz") == 0 && it.option_count() == 0);
    ElcChWriter w;
    elc_ui_encode(it, w);
    CHECK(w.size() == sizeof(freq) && std::memcmp(w.data(), freq, sizeof(freq)) == 0);

    static const uint8_t preset[] = {0x01, 0x09, 0xff, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x50, 0x72, 0x65, 0x73, 0x65, 0x74, 0x00, 0x00, 0x31, 0x20, 0x20, 0x31,
                                     0x30, 0x31, 0x2e, 0x31, 0x20, 0x46, 0x4d, 0x0a, 0x32, 0x20, 0x20, 0x2d, 0x2d};
    t.data = preset;
    t.len = sizeof(preset);
    CHECK(elc_ui_decode(t, it));
    char o[32];
    CHECK(it.kind == ELC_UI_CHOICE && it.unit[0] == 0 && it.option_count() == 2);
    CHECK(it.option(0, o, sizeof(o)) && std::strcmp(o, "1  101.1 FM") == 0);
    CHECK(it.option(1, o, sizeof(o)) && std::strcmp(o, "2  --") == 0);
    CHECK(!it.option(2, o, sizeof(o)));
  }
  {
    ElcChWriter w;
    elc_ui_encode(ElcUiSet{0x15, 2, 947}, w);
    ElcUiSet s;
    CHECK(elc_ui_decode(as_transfer(ELC_CH_UI, 1, w), s) && s.dest == 0x15 && s.item == 2 && s.value == 947);
    CHECK(as_transfer(ELC_CH_UI, 1, w).for_me(0x15) && !as_transfer(ELC_CH_UI, 1, w).for_me(0x16));
    ElcUiValue bad;
    CHECK(!elc_ui_decode(as_transfer(ELC_CH_UI, 1, w), bad));  // wrong opcode
    char f[16];
    elc_ui_format(947, -1, f, sizeof(f));
    CHECK(std::strcmp(f, "94.7") == 0);
    elc_ui_format(-5, -2, f, sizeof(f));
    CHECK(std::strcmp(f, "-0.05") == 0);
    elc_ui_format(1070, 0, f, sizeof(f));
    CHECK(std::strcmp(f, "1070") == 0);
  }
  {
    ElcSensorDefine d;
    d.sensor = 3;
    d.quantity = ELC_Q_TEMPERATURE;
    d.exponent = -1;
    d.flags = ELC_SENSOR_F_LOG | ELC_SENSOR_F_GAUGE;
    d.period_ms = 200;
    d.disp_min = 0;
    d.disp_max = 1300;
    d.warn_hi = 1150;
    std::strcpy(d.name, "Engine temp");
    ElcChWriter w;
    elc_sensor_encode(d, w);
    CHECK(w.size() == 1 + 30 + 11);  // op, 6 bytes, six s32, name
    ElcSensorDefine e;
    CHECK(elc_sensor_decode(as_transfer(ELC_CH_SENSOR, 1, w), e));
    CHECK(e.sensor == 3 && e.exponent == -1 && e.period_ms == 200 && e.warn_hi == 1150 && e.caution_lo == ELC_CH_NONE32);
    CHECK(std::strcmp(e.name, "Engine temp") == 0);
    ElcChWriter v;
    elc_sensor_encode(ElcSensorValue{3, 1043, 0}, v);
    ElcSensorValue sv;
    CHECK(v.size() == 7 && elc_sensor_decode(as_transfer(ELC_CH_SENSOR, 1, v), sv) && sv.value == 1043);
    CHECK(!elc_sensor_decode(as_transfer(ELC_CH_UI, 1, v), sv));  // wrong channel
  }
  {
    ElcAlertDefine a;
    a.alert = 4;
    a.severity = ELC_SEV_WARNING;
    a.flags = ELC_ALERT_F_ACK;
    a.icon = 2;
    std::strcpy(a.text, "Oil pressure low");
    ElcChWriter w;
    elc_alert_encode(a, w);
    ElcAlertDefine b;
    CHECK(elc_alert_decode(as_transfer(ELC_CH_ALERT, 1, w), b) && b.alert == 4 && b.severity == 3 &&
          std::strcmp(b.text, "Oil pressure low") == 0);
    ElcChWriter s;
    elc_alert_encode(ElcAlertState{1u << 4, 2}, s);
    ElcAlertState st;
    CHECK(elc_alert_decode(as_transfer(ELC_CH_ALERT, 1, s), st) && st.active == 16 && st.generation == 2);
  }
  {
    ElcChWriter w;
    elc_sys_encode(ElcSysTime{1790000000u, -20, ELC_TIME_SRC_GPS}, w);
    ElcSysTime t;
    CHECK(w.size() == 7 && elc_sys_decode(as_transfer(ELC_CH_SYSTEM, 1, w), t) && t.utc == 1790000000u && t.offset_q == -20);
    ElcChWriter v;
    elc_sys_encode(ElcSysVehicle{ELC_VEH_IGNITION | ELC_VEH_NIGHT, 40, 885}, v);
    ElcSysVehicle vv;
    CHECK(elc_sys_decode(as_transfer(ELC_CH_SYSTEM, 1, v), vv) && vv.flags == 0x41 && vv.backlight == 40 && vv.speed_x10 == 885);
    ElcSysPrefs pp;
    CHECK(!elc_sys_decode(as_transfer(ELC_CH_SYSTEM, 1, v), pp));
  }
  {
    ElcChWriter w;
    elc_input_encode(ElcInputKey{ELC_KEY_NEXT, ELC_GEST_CLICK, 9}, w);
    ElcInputKey k;
    CHECK(elc_input_decode(as_transfer(ELC_CH_INPUT, 0x13, w), k) && k.key == 17 && k.seq == 9);
    ElcChWriter e;
    elc_input_encode(ElcInputEncoder{ELC_ENC_VOLUME, -300}, e);
    ElcInputEncoder en;
    CHECK(elc_input_decode(as_transfer(ELC_CH_INPUT, 0x13, e), en) && en.encoder == 1 && en.position == -300);
  }
  {
    ElcChWriter w;
    elc_audio_encode(ElcAudioStatus{60, ELC_AUDIO_F_PLAYING, 0, 0, 15}, w);
    ElcAudioStatus s;
    CHECK(elc_audio_decode(as_transfer(ELC_CH_AUDIO_CMD, 0x14, w), s) && s.volume == 60 && s.zones == 15);
    ElcChWriter m;
    elc_audio_encode(ElcAudioMedia{0x00, ELC_MEDIA_NEXT}, m);
    ElcAudioMedia md;
    CHECK(elc_audio_decode(as_transfer(ELC_CH_AUDIO_CMD, 0x01, m), md) && md.action == ELC_MEDIA_NEXT);
    CHECK(as_transfer(ELC_CH_AUDIO_CMD, 0x01, m).for_me(0x14));  // dest 0 = everyone
    ElcChWriter old;  // a sink that sends no source fields yet
    old.u8(ELC_AUDIO_STATUS).u8(50).u8(0).u8(0).u8(0).u8(15);
    CHECK(elc_audio_decode(as_transfer(ELC_CH_AUDIO_CMD, 0x14, old), s) && s.source == 0 && s.volume == 50);
    ElcChWriter st;
    elc_audio_encode(ElcAudioStatus{60, 0, 0, 0, 15, ELC_SRC_RADIO, 0x15}, st);
    CHECK(elc_audio_decode(as_transfer(ELC_CH_AUDIO_CMD, 0x14, st), s) && s.source == ELC_SRC_RADIO && s.source_node == 0x15);
    ElcChWriter mv;
    elc_audio_encode(ElcAudioMedia{0x14, ELC_MEDIA_PRESET_STORE, 3}, mv);
    CHECK(elc_audio_decode(as_transfer(ELC_CH_AUDIO_CMD, 0x13, mv), md) && md.action == 10 && md.value == 3);
    ElcAudioNow now;
    now.source = ELC_SRC_RADIO;
    now.flags = ELC_NOW_F_STEREO | ELC_NOW_F_RDS;
    now.preset = 2;
    now.signal = 4;
    std::strcpy(now.line1, "94.7 FM  THE WAVE");
    std::strcpy(now.line2, "Now: Sea Breeze - Coastline");
    ElcChWriter nw;
    elc_audio_encode(now, nw);
    CHECK(nw.size() == 1 + 4 + 1 + 17 + 1 + 27);
    ElcAudioNow back;
    CHECK(elc_audio_decode(as_transfer(ELC_CH_AUDIO_CMD, 0x15, nw), back) && back.preset == 2 && back.signal == 4);
    CHECK(std::strcmp(back.line1, now.line1) == 0 && std::strcmp(back.line2, now.line2) == 0);
    ElcChWriter sw;
    elc_audio_encode(ElcAudioSource{0x14, 0}, sw);
    ElcAudioSource src;
    CHECK(elc_audio_decode(as_transfer(ELC_CH_AUDIO_CMD, 0x13, sw), src) && src.dest == 0x14 && src.source == 0);
  }
  {
    ElcLogLine l;
    l.level = ELC_LOG_WARN;
    std::strcpy(l.text, "tuned 94.7 FM");
    ElcChWriter w;
    elc_log_encode(l, w);
    ElcLogLine m;
    CHECK(elc_log_decode(as_transfer(ELC_CH_LOG, 0x15, w), m) && m.level == 2 && std::strcmp(m.text, "tuned 94.7 FM") == 0);
  }
}

/** The audio bridge re-flashed: same node, fw number and generation, but a new type and page. */
static void test_accessory_refresh() {
  std::printf("accessory pages: asked again on every ANNOUNCE, forgotten when the board or firmware changes\n");
  Bus bus;
  ElcChannels ch;
  ElcNetAnnounce me;
  me.type = ELC_TYPE_CONSOLE;
  ch.begin(0x13, me, bus_send, &bus);
  accessory_can_begin(&ch);
  auto announce = [&](uint8_t type) {
    ElcNetAnnounce a;
    a.type = type;
    a.product = ELC_PRODUCT_AUDIO_BRIDGE;
    a.fw = 1;
    a.uid = 0x1234;
    a.generation = 1;
    a.produces = elc_ch_bit(ELC_CH_UI);
    static uint8_t b[64];
    const size_t n = elc_net_encode_announce(a, b, sizeof(b));
    ElcChTransfer t;
    t.base = ELC_CH_NET;
    t.src = 0x10;
    t.data = b;
    t.len = n;
    bus.frames.clear();
    accessory_can_transfer(t, 100);
  };
  auto queried = [&]() {
    for (const ElcCanFrame &f : bus.frames) {
      if (f.id == ELC_CH_NET + 0x13 && f.len >= 3 && f.data[0] == ELC_NET_QUERY && f.data[1] == 0x10) return true;
    }
    return false;
  };
  auto item = [&](uint8_t n, const char *label) {
    ElcUiItem it;
    it.item = n;
    it.parent = ELC_UI_TOP;
    it.kind = ELC_UI_NUMBER;
    std::snprintf(it.label, sizeof(it.label), "%s", label);
    ElcChWriter w;
    elc_ui_encode(it, w);
    accessory_can_transfer(as_transfer(ELC_CH_UI, 0x10, w), 100);
  };
  const AccItem *rows[8];
  item(1, "Chime volume");  // the old firmware's page (no announce seen yet)
  CHECK(accessory_can_items(0x10, ELC_UI_TOP, false, rows, 8) == 1);
  announce(ELC_TYPE_AUDIO_BRIDGE);  // the old firmware announces: asked again
  CHECK(queried());
  item(1, "Chime volume");
  announce(ELC_TYPE_AUDIO_BRIDGE);  // the same board and firmware again (a QUERY): asked again, nothing forgotten
  CHECK(queried() && accessory_can_items(0x10, ELC_UI_TOP, false, rows, 8) == 1);
  announce(ELC_TYPE_AUDIO_SINK);  // re-flashed as the audio sink: the old page goes, the new one is asked for
  CHECK(queried() && accessory_can_items(0x10, ELC_UI_TOP, false, rows, 8) == 0);
  item(1, "Chime volume");
  item(3, "Balance");
  CHECK(accessory_can_items(0x10, ELC_UI_TOP, false, rows, 8) == 2);
}

int main() {
  test_crc_and_frames();
  test_reassembly();
  test_net_codecs();
  test_service();
  test_messages();
  test_accessory_refresh();
  if (g_failures) {
    std::printf("%d FAILED\n", g_failures);
    return 1;
  }
  std::printf("all channel tests passed\n");
  return 0;
}
