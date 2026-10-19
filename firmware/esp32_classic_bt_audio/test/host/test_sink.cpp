// Host tests for the audio card as the audio sink (sink.cpp): sources (Bluetooth, the radio, off; no aux), Bluetooth paused and
// resumed, volume and mute, MEDIA forwarded to the radio, RET arbitration on NET RAIL, STATUS, and the card's page
// (chime volume, fader, balance, the equaliser's presets and Custom curve).

#include "sink.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio_dsp.h"

static int g_fail = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
      g_fail++;                                                     \
    }                                                               \
  } while (0)

static uint32_t T = 1000;

struct Bus {
  std::vector<ElcCanFrame> frames;
};
static bool capture(const ElcCanFrame &f, void *ctx) {
  static_cast<Bus *>(ctx)->frames.push_back(f);
  return true;
}

struct Io : SinkIo {
  ElcChannels *ch = nullptr;
  std::vector<uint8_t> media;
  bool playing = false;
  std::vector<uint8_t> alive;  // nodes whose heartbeat is current
  uint8_t chimes = 0;
  bool ducking = false;
  int previews = 0, applies = 0, saves = 0;
  SinkSettings saved;
  std::vector<std::string> lines;
  void send(uint16_t base, const ElcChWriter &w) override { ch->send(base, w); }
  void bt_media(uint8_t a) override {
    media.push_back(a);
    if (a == ELC_MEDIA_PLAY) playing = true;
    if (a == ELC_MEDIA_PAUSE) playing = false;
  }
  bool bt_playing() override { return playing; }
  bool node_alive(uint8_t n) override {
    for (uint8_t a : alive) {
      if (a == n) return true;
    }
    return false;
  }
  uint8_t chimes_active() override { return chimes; }
  bool chimes_ducking() override { return ducking; }
  void chime_preview(uint8_t) override { previews++; }
  void apply() override { applies++; }
  void save(const SinkSettings &s) override {
    saves++;
    saved = s;
  }
  void log(const char *l) override { lines.push_back(l); }
};

/** The card (node 0x14) and the nodes around it, each a channel node whose frames reach the others. */
struct Rig {
  ElcChannels card, console, radio, voice;
  Bus card_out, console_out, radio_out, voice_out;
  Io io;
  AudioSink sink;

  explicit Rig(SinkSettings s = SinkSettings()) {
    ElcNetAnnounce me;
    me.type = ELC_TYPE_AUDIO_SINK;
    me.product = ELC_PRODUCT_AUDIO_CARD;
    me.consumes = elc_ch_bit(ELC_CH_AUDIO_CMD) | elc_ch_bit(ELC_CH_UI);
    std::strcpy(me.name, "Audio card");
    card.begin(0x14, me, capture, &card_out);
    card.onTransfer([](const ElcChTransfer &t, void *c) { static_cast<AudioSink *>(c)->transfer(t, T); }, &sink);
    io.ch = &card;
    ElcNetAnnounce c;
    c.type = ELC_TYPE_CONSOLE;
    std::strcpy(c.name, "Console");
    console.begin(0x13, c, capture, &console_out);
    AudioSink::Caps caps;
    caps.rear = true;
    caps.slot = 1;
    sink.begin(&card, &io, caps, s, T);
  }

  void node(ElcChannels &n, Bus &b, uint8_t id, uint8_t type, const char *name) {
    ElcNetAnnounce a;
    a.type = type;
    std::strcpy(a.name, name);
    n.begin(id, a, capture, &b);
    n.poll(0);
    deliver(b);
    io.alive.push_back(id);
  }
  void add_radio() { node(radio, radio_out, 0x15, ELC_TYPE_RADIO, "Radio"); }
  void add_voice() { node(voice, voice_out, 0x18, ELC_TYPE_VOICE, "Voice"); }

  void deliver(Bus &b) {
    for (const ElcCanFrame &f : b.frames) card.feed(f, T);
    b.frames.clear();
  }

  /** The card's transfers on a channel since the last call for that channel. */
  std::vector<std::vector<uint8_t>> heard[32];
  ElcChRx rx;
  std::vector<std::vector<uint8_t>> sent(uint16_t base) {
    ElcChTransfer t;
    for (const ElcCanFrame &f : card_out.frames) {
      if (rx.feed(f, 0, t)) heard[(t.base >> 5) & 31].emplace_back(t.data, t.data + t.len);
    }
    card_out.frames.clear();
    std::vector<std::vector<uint8_t>> r;
    r.swap(heard[(base >> 5) & 31]);
    return r;
  }
  std::vector<uint8_t> last_status() {
    std::vector<uint8_t> s;
    for (auto &m : sent(ELC_CH_AUDIO_CMD)) {
      if (m[0] == ELC_AUDIO_STATUS) s = m;
    }
    if (s.size() < 8) s.assign(8, 0xEE);  // none sent: a CHECK on it fails, not the test
    return s;
  }

  void cmd(ElcChannels &from, Bus &b, uint16_t base, const ElcChWriter &w) {
    from.send(base, w);
    deliver(b);
  }
  void source(uint8_t s) {
    ElcChWriter w;
    elc_audio_encode(ElcAudioSource{0x14, s}, w);
    cmd(console, console_out, ELC_CH_AUDIO_CMD, w);
  }
  void media(uint8_t a, uint8_t v = 0) {
    ElcChWriter w;
    elc_audio_encode(ElcAudioMedia{0x14, a, v}, w);
    cmd(console, console_out, ELC_CH_AUDIO_CMD, w);
  }
  void volume(uint8_t action, int8_t v) {
    ElcChWriter w;
    elc_audio_encode(ElcAudioVolume{0x14, action, v}, w);
    cmd(console, console_out, ELC_CH_AUDIO_CMD, w);
  }
  void rail(ElcChannels &from, Bus &b, uint8_t state) {
    ElcNetRail r;
    r.dest = 0x14;
    r.slot = 2;
    r.state = state;
    uint8_t buf[8];
    ElcChWriter w;
    w.bytes(buf, elc_net_encode_rail(r, buf, sizeof(buf)));
    cmd(from, b, ELC_CH_NET, w);
  }
  void set(uint8_t item, int32_t v) {
    ElcChWriter w;
    elc_ui_encode(ElcUiSet{0x14, item, v}, w);
    cmd(console, console_out, ELC_CH_UI, w);
  }
  /** RAIL answers the card sent: (dest, state). */
  std::vector<std::pair<uint8_t, uint8_t>> rails() {
    std::vector<std::pair<uint8_t, uint8_t>> r;
    for (auto &m : sent(ELC_CH_NET)) {
      ElcNetRail x;
      if (elc_net_decode_rail(m.data(), m.size(), x)) r.emplace_back(x.dest, x.state);
    }
    return r;
  }
};

static void test_sources() {
  std::printf("sources: next round Bluetooth, radio, off; aux refused; Bluetooth paused and resumed\n");
  Rig r;
  r.card.poll(0);
  r.sink.tick(T);
  auto st = r.last_status();
  CHECK(st.size() >= 8 && st[6] == ELC_SRC_BLUETOOTH && st[7] == 0x14);
  CHECK(!(st[2] & ELC_AUDIO_F_AUX));  // no aux in
  CHECK(st[5] == 0x0F);  // four zones: the rear DAC is fitted
  r.source(0);  // no radio yet: Bluetooth -> off
  CHECK(r.sink.source() == ELC_SRC_OFF && r.sink.music() == SinkMusic::Off);
  r.source(0);
  CHECK(r.sink.source() == ELC_SRC_BLUETOOTH);
  r.source(ELC_SRC_RADIO);  // not heard: refused
  CHECK(r.sink.source() == ELC_SRC_BLUETOOTH);
  r.add_radio();
  r.io.playing = true;
  r.source(0);
  CHECK(r.sink.source() == ELC_SRC_RADIO);
  CHECK(r.io.media.size() == 1 && r.io.media[0] == ELC_MEDIA_PAUSE);  // the phone paused on leaving Bluetooth
  st = r.last_status();
  CHECK(st[6] == ELC_SRC_RADIO && st[7] == 0x15);  // STATUS names the radio: it asks for RET
  r.source(ELC_SRC_REQ_OFF);
  CHECK(r.sink.source() == ELC_SRC_OFF && r.io.media.size() == 1);
  r.source(ELC_SRC_BLUETOOTH);
  CHECK(r.io.media.size() == 2 && r.io.media[1] == ELC_MEDIA_PLAY);  // it was playing: it plays again
  // Aux asked for: refused, the source stays. A saved aux (older firmware) boots to Bluetooth.
  Rig d;
  d.source(ELC_SRC_AUX);
  CHECK(d.sink.source() == ELC_SRC_BLUETOOTH);
  SinkSettings saved;
  saved.source = ELC_SRC_AUX;
  Rig e(saved);
  CHECK(e.sink.source() == ELC_SRC_BLUETOOTH);
}

static void test_boot_radio() {
  std::printf("booted to the radio: waits for it, else Bluetooth\n");
  SinkSettings s;
  s.source = ELC_SRC_RADIO;
  {
    Rig r(s);
    CHECK(r.sink.source() == ELC_SRC_RADIO);
    T += 2000;
    r.add_radio();
    T += 6000;
    r.sink.tick(T);
    CHECK(r.sink.source() == ELC_SRC_RADIO);
  }
  {
    Rig r(s);
    T += 7000;
    r.sink.tick(T);
    CHECK(r.sink.source() == ELC_SRC_BLUETOOTH);
  }
}

static void test_volume_media() {
  std::printf("volume set / step / mute; MEDIA to the phone or the radio\n");
  Rig r;
  r.volume(0, 100);
  CHECK(r.sink.volume15() == 15);
  r.volume(0, 53);
  CHECK(r.sink.volume15() == 8);
  r.volume(1, 2);  // a small step still moves a notch
  CHECK(r.sink.volume15() == 9);
  r.volume(1, -20);
  CHECK(r.sink.volume15() == 6);
  r.volume(2, 0);
  CHECK(r.sink.muted());
  r.sink.tick(T);
  auto st = r.last_status();
  CHECK((st[2] & ELC_AUDIO_F_MUTED) && st[1] == 40);
  r.volume(0, 60);  // a new volume unmutes
  CHECK(!r.sink.muted() && r.sink.volume15() == 9);
  r.media(ELC_MEDIA_TUNE_UP);  // Bluetooth: a track
  CHECK(r.io.media.back() == ELC_MEDIA_NEXT);
  r.add_radio();
  r.source(ELC_SRC_RADIO);
  r.sent(ELC_CH_AUDIO_CMD);
  const size_t before = r.io.media.size();
  r.media(ELC_MEDIA_SEEK_UP);
  CHECK(r.io.media.size() == before);
  bool fwd = false;
  for (auto &m : r.sent(ELC_CH_AUDIO_CMD)) {
    if (m[0] == ELC_AUDIO_MEDIA && m[1] == 0x15 && m[2] == ELC_MEDIA_SEEK_UP) fwd = true;
  }
  CHECK(fwd);  // forwarded to the radio
  r.sink.set_volume15(3, T);  // the old Bluetooth frames
  CHECK(r.sink.volume15() == 3);
  T += 2500;
  r.sink.tick(T);
  CHECK(r.io.saves >= 1 && r.io.saved.volume == 3 && r.io.saved.source == ELC_SRC_RADIO);
}

static void test_rail() {
  std::printf("RET: granted, denied, withdrawn for a voice, freed when the holder goes quiet\n");
  Rig r;
  r.add_radio();
  r.add_voice();
  r.source(ELC_SRC_RADIO);
  r.sent(ELC_CH_NET);
  r.rail(r.radio, r.radio_out, 0);
  auto a = r.rails();
  CHECK(a.size() == 1 && a[0].first == 0x15 && a[0].second == 1);
  CHECK(r.sink.holder() == 0x15 && !r.sink.rail_voice() && r.sink.music() == SinkMusic::Rail);
  r.sink.tick(T);
  auto st = r.last_status();
  CHECK(st[2] & ELC_AUDIO_F_PLAYING);
  r.rail(r.voice, r.voice_out, 0);  // a voice card outranks the radio
  a = r.rails();
  CHECK(a.size() == 2 && a[0] == std::make_pair((uint8_t)0x15, (uint8_t)2) && a[1] == std::make_pair((uint8_t)0x18, (uint8_t)1));
  CHECK(r.sink.holder() == 0x18 && r.sink.rail_voice());
  r.sink.tick(T);
  st = r.last_status();
  CHECK(st[2] & ELC_AUDIO_F_DUCKED);
  r.rail(r.radio, r.radio_out, 0);  // the radio asks again: denied while the voice talks
  a = r.rails();
  CHECK(a.size() == 1 && a[0] == std::make_pair((uint8_t)0x15, (uint8_t)2));
  r.rail(r.voice, r.voice_out, 2);  // released
  CHECK(r.sink.holder() == 0);
  r.rail(r.radio, r.radio_out, 0);
  CHECK(r.sink.holder() == 0x15);
  r.io.alive.clear();  // the radio's heartbeat stops
  T += 3500;
  r.sink.tick(T);
  CHECK(r.sink.holder() == 0);
}

static void test_page() {
  std::printf("the page: items, chime volume, fader, the equaliser's presets and Custom\n");
  Rig r;
  r.sink.describe_ui();
  int items = 0, values = 0;
  for (auto &m : r.sent(ELC_CH_UI)) {
    items += m[0] == ELC_UI_ITEM;
    values += m[0] == ELC_UI_VALUE;
  }
  CHECK(items == 15 && values == 14);  // chime, fader, balance, the group, the preset, ten bands
  auto factory = [&]() {  // the card's ANNOUNCE flag bit0
    r.card.announce();
    int f = -1;
    for (auto &m : r.sent(ELC_CH_NET)) {
      ElcNetAnnounce a;
      if (m[0] == ELC_NET_ANNOUNCE && elc_net_decode_announce(m.data(), m.size(), a)) f = a.flags & 1;
    }
    return f;
  };
  CHECK(factory() == 1);  // factory defaults until something is set
  r.set(ELC_SINK_ITEM_CHIME_VOL, 12);
  CHECK(r.sink.settings().chime_volume == 12 && r.io.previews == 1);
  r.set(ELC_SINK_ITEM_CHIME_VOL, 0);  // out of range
  CHECK(r.sink.settings().chime_volume == 12);
  r.set(ELC_SINK_ITEM_FADER, -4);
  CHECK(r.sink.settings().fader == -4);
  r.sent(ELC_CH_UI);
  // A preset: its curve is the EQ, and every band's VALUE goes out.
  r.set(ELC_SINK_ITEM_EQ_PRESET, 4);  // Rock
  int8_t db[10];
  r.sink.eq_db(db);
  CHECK(std::memcmp(db, dsp::Eq10::kPresetDb[4], 10) == 0);
  int band_values = 0;
  for (auto &m : r.sent(ELC_CH_UI)) band_values += m[0] == ELC_UI_VALUE;
  CHECK(band_values == 11);
  // A band moved on Rock: Custom, starting from Rock's curve.
  r.set(ELC_SINK_ITEM_EQ_BAND0 + 2, -7);
  CHECK(r.sink.settings().eq_preset == ELC_EQ_PRESET_CUSTOM);
  r.sink.eq_db(db);
  CHECK(db[0] == 5 && db[2] == -7 && db[9] == 4);
  // Flat, then back to Custom: the curve was kept.
  r.set(ELC_SINK_ITEM_EQ_PRESET, ELC_EQ_PRESET_FLAT);
  r.sink.eq_db(db);
  CHECK(db[2] == 0);
  r.set(ELC_SINK_ITEM_EQ_PRESET, ELC_EQ_PRESET_CUSTOM);
  r.sink.eq_db(db);
  CHECK(db[2] == -7);
  r.set(ELC_SINK_ITEM_EQ_BAND0, 13);  // out of range
  r.sink.eq_db(db);
  CHECK(db[0] == 5);
  CHECK(factory() == 0);
  T += 2500;
  r.sink.tick(T);
  CHECK(r.io.saved.eq_preset == ELC_EQ_PRESET_CUSTOM && r.io.saved.eq_custom[2] == -7 && !r.io.saved.factory);
}

int main() {
  test_sources();
  test_boot_radio();
  test_volume_media();
  test_rail();
  test_page();
  if (g_fail) {
    std::printf("%d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("sink: all passed\n");
  return 0;
}
