// Host tests for the alert manager (elc_can/src/elc_alerts.cpp). Run: ./run_tests.sh

#include <elc_alerts.h>
#include <elc_chan.h>
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

// What the center put on the bus, as transfers.
struct Sent {
  uint16_t base;
  std::vector<uint8_t> data;
};
struct Bus {
  std::vector<ElcCanFrame> frames;
  std::vector<Sent> take() {
    std::vector<Sent> out;
    ElcChRx rx;
    ElcChTransfer t;
    for (const ElcCanFrame &f : frames) {
      if (rx.feed(f, 0, t)) out.push_back({t.base, std::vector<uint8_t>(t.data, t.data + t.len)});
    }
    frames.clear();
    return out;
  }
};
static bool bus_send(const ElcCanFrame &f, void *ctx) {
  ((Bus *)ctx)->frames.push_back(f);
  return true;
}

static int count(const std::vector<Sent> &s, uint16_t base, uint8_t op) {
  int k = 0;
  for (const Sent &x : s) k += x.base == base && !x.data.empty() && x.data[0] == op;
  return k;
}
static const Sent *last(const std::vector<Sent> &s, uint16_t base, uint8_t op) {
  const Sent *p = nullptr;
  for (const Sent &x : s) {
    if (x.base == base && !x.data.empty() && x.data[0] == op) p = &x;
  }
  return p;
}
static ElcChTransfer as_transfer(uint16_t base, uint8_t src, const Sent &s) {
  ElcChTransfer t;
  t.base = base;
  t.src = src;
  t.data = s.data.data();
  t.len = s.data.size();
  return t;
}

// A remote node's transfer.
struct Msg {
  ElcChWriter w;
  ElcChTransfer t(uint16_t base, uint8_t src) const {
    ElcChTransfer x;
    x.base = base;
    x.src = src;
    x.data = w.data();
    x.len = w.size();
    return x;
  }
};
static void define(ElcAlertManager &m, uint8_t node, uint8_t alert, uint8_t sev, uint8_t flags, uint16_t sound,
                   const char *text, uint32_t now) {
  ElcAlertDefine d;
  d.alert = alert;
  d.severity = sev;
  d.flags = flags;
  d.sound = sound;
  elc_ch_copy_text(d.text, sizeof(d.text), text);
  Msg msg;
  elc_alert_encode(d, msg.w);
  m.feed(msg.t(ELC_CH_ALERT, node), now);
}
static void state(ElcAlertManager &m, uint8_t node, uint32_t active, uint8_t gen, uint32_t now) {
  Msg msg;
  elc_alert_encode(ElcAlertState{active, gen}, msg.w);
  m.feed(msg.t(ELC_CH_ALERT, node), now);
}

struct Rig {
  Bus bus;
  ElcChannels ch;
  ElcAlertManager m;
  Rig() {
    ElcNetAnnounce me;
    me.type = ELC_TYPE_DISPLAY;
    ch.begin(0x01, me, bus_send, &bus);
    m.begin(&ch);
  }
};

static bool play_is(const Sent *p, uint16_t sound, uint8_t prio, uint8_t repeat) {
  if (!p) return false;
  ElcAudioPlay v;
  return elc_audio_decode(as_transfer(ELC_CH_AUDIO_CMD, 0x01, *p), v) && v.sound == sound &&
         (v.zones_prio & 0x0F) == prio && v.repeat == repeat;
}

static void test_collect_and_chime() {
  std::printf("collect, describe on demand, one-shot chimes\n");
  Rig r;
  // STATE before DEFINE: not listed, the node is asked for its descriptions.
  state(r.m, 0x04, 1u << 0, 1, 100);
  r.m.poll(100);
  std::vector<Sent> s = r.bus.take();
  const Sent *q = last(s, ELC_CH_NET, ELC_NET_QUERY);
  CHECK(q && q->data.size() == 3 && q->data[1] == 0x04 && q->data[2] == ELC_NET_Q_ALERTS);
  CHECK(r.m.top() == nullptr);
  CHECK(count(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY) == 0);

  define(r.m, 0x04, 0, ELC_SEV_CAUTION, 0, 0, "Trans fluid hot", 120);
  r.m.poll(130);
  s = r.bus.take();
  CHECK(play_is(last(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY), 3, 3, 1));  // caution chime once, priority 3
  const ElcAlertManager::Alert *t = r.m.top();
  CHECK(t && t->node == 0x04 && t->alert == 0 && std::strcmp(t->text, "Trans fluid hot") == 0);
  r.m.poll(200);
  state(r.m, 0x04, 1u << 0, 1, 900);
  r.m.poll(1200);
  s = r.bus.take();
  CHECK(count(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY) == 0);  // once per activation

  // SUMMARY: one active caution, unacknowledged.
  const Sent *sm = last(s, ELC_CH_ALERT, ELC_ALERT_SUMMARY);
  ElcAlertSummary v;
  CHECK(sm && elc_alert_decode(as_transfer(ELC_CH_ALERT, 0x01, *sm), v) && v.top_sev == 2 && v.top_unacked == 2 &&
        v.count == 1 && v.top_node == 0x04 && v.top_alert == 0);

  // Cleared, then active again: a new activation chimes again.
  state(r.m, 0x04, 0, 1, 1300);
  CHECK(r.m.top() == nullptr);
  state(r.m, 0x04, 1u << 0, 1, 1400);
  r.m.poll(1400);
  s = r.bus.take();
  CHECK(play_is(last(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY), 3, 3, 1));

  // Info and silent alerts never chime; an advisory gets the soft chime.
  define(r.m, 0x04, 1, ELC_SEV_INFO, 0, 0, "Calibrate", 1500);
  define(r.m, 0x04, 2, ELC_SEV_CAUTION, ELC_ALERT_F_SILENT, 0, "Quiet", 1500);
  define(r.m, 0x04, 3, ELC_SEV_CAUTION, 0, 0xFFFF, "No sound", 1500);
  define(r.m, 0x04, 4, ELC_SEV_ADVISORY, 0, 0, "Advice", 1500);
  state(r.m, 0x04, 0x1F, 1, 1500);
  r.m.poll(1500);
  s = r.bus.take();
  CHECK(count(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY) == 1);
  CHECK(play_is(last(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY), 2, 2, 1));
}

static void test_warning_rings_until_ack() {
  std::printf("warning rings until acknowledged\n");
  Rig r;
  define(r.m, 0x14, 0, ELC_SEV_WARNING, ELC_ALERT_F_ACK, 0, "Trans temp 260F", 0);
  state(r.m, 0x14, 1, 1, 0);
  r.m.poll(10);
  std::vector<Sent> s = r.bus.take();
  CHECK(play_is(last(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY), 4, 4, 0));  // repeat 0: until STOP
  CHECK(r.m.ringing() == 4);
  r.m.poll(500);
  s = r.bus.take();
  CHECK(count(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY) == 0);

  // The sink cut it short (DONE for our sound, after the guard): played again.
  {
    Msg done;
    done.w.u8(ELC_AUDIO_DONE).u8(0x01).u16(4);
    r.m.feed(done.t(ELC_CH_AUDIO_CMD, 0x14), 2000);
    r.m.poll(2000);
    s = r.bus.take();
    CHECK(play_is(last(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY), 4, 4, 0));
  }
  // Another node's DONE does nothing.
  {
    Msg done;
    done.w.u8(ELC_AUDIO_DONE).u8(0x05).u16(4);
    r.m.feed(done.t(ELC_CH_AUDIO_CMD, 0x14), 3000);
    r.m.poll(3000);
    s = r.bus.take();
    CHECK(count(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY) == 0);
  }

  // The driver acknowledges: ACK to the source, the chime stops, it stays listed (still active) but not on top.
  CHECK(r.m.ackTop(3100));
  s = r.bus.take();
  const Sent *k = last(s, ELC_CH_ALERT, ELC_ALERT_ACK);
  ElcAlertAck a;
  CHECK(k && elc_alert_decode(as_transfer(ELC_CH_ALERT, 0x01, *k), a) && a.dest == 0x14 && a.alert == 0);
  const Sent *st = last(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_STOP);
  ElcAudioStop sv;
  CHECK(st && elc_audio_decode(as_transfer(ELC_CH_AUDIO_CMD, 0x01, *st), sv) && sv.sound == 4);
  CHECK(r.m.ringing() == 0 && r.m.top() == nullptr);
  const ElcAlertManager::Alert *l[4];
  CHECK(r.m.list(l, 4) == 1 && l[0]->acked);
  CHECK(!r.m.ackTop(3200));
  r.m.poll(3300);
  s = r.bus.take();
  CHECK(count(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY) == 0);
}

static void test_order_latch_and_lost() {
  std::printf("ordering, latching, source lost, generation\n");
  Rig r;
  define(r.m, 0x04, 0, ELC_SEV_ADVISORY, 0, 0, "Low fuel", 0);
  define(r.m, 0x04, 1, ELC_SEV_WARNING, 0, 0, "Oil", 0);
  define(r.m, 0x04, 2, ELC_SEV_CAUTION, ELC_ALERT_F_LATCH, 0, "Overrev", 0);
  state(r.m, 0x04, 0x7, 1, 0);
  r.m.poll(0);
  const ElcAlertManager::Alert *l[8];
  CHECK(r.m.list(l, 8) == 3 && l[0]->alert == 1 && l[1]->alert == 2 && l[2]->alert == 0);

  // A latching alert that clears unacknowledged stays listed until acknowledged.
  state(r.m, 0x04, 0x3, 1, 500);
  CHECK(r.m.list(l, 8) == 3);
  bool latched = false;
  for (int i = 0; i < 3; i++) latched |= l[i]->alert == 2 && l[i]->latched && !l[i]->active;
  CHECK(latched);
  CHECK(r.m.ack(0x04, 2, 600));
  CHECK(r.m.list(l, 8) == 2);

  // STATE stops for more than 3 s: the node is lost (its alerts stay listed); back when STATE returns.
  r.m.poll(3400);
  CHECK(!r.m.nodeLost(0x04));
  r.m.poll(3600);
  CHECK(r.m.nodeLost(0x04) && r.m.list(l, 8) == 2);
  state(r.m, 0x04, 0x3, 1, 3700);
  CHECK(!r.m.nodeLost(0x04));

  // A new generation: the node is asked for its descriptions again.
  r.bus.take();
  state(r.m, 0x04, 0x3, 2, 3800);
  std::vector<Sent> s = r.bus.take();
  CHECK(count(s, ELC_CH_NET, ELC_NET_QUERY) == 1);
}

static void test_own_alerts() {
  std::printf("own alerts on the channel\n");
  Rig r;
  ElcAlertDefine d;
  d.alert = 1;
  d.severity = ELC_SEV_WARNING;
  d.flags = ELC_ALERT_F_ACK;
  d.icon = 2;
  elc_ch_copy_text(d.text, sizeof(d.text), "Low oil pressure");
  r.m.defineOwn(d);
  r.m.setOwn(1, true);
  r.m.poll(0);
  std::vector<Sent> s = r.bus.take();
  const Sent *df = last(s, ELC_CH_ALERT, ELC_ALERT_DEFINE);
  ElcAlertDefine back;
  CHECK(df && elc_alert_decode(as_transfer(ELC_CH_ALERT, 0x01, *df), back) && back.alert == 1 &&
        std::strcmp(back.text, "Low oil pressure") == 0);
  const Sent *st = last(s, ELC_CH_ALERT, ELC_ALERT_STATE);
  ElcAlertState sv;
  CHECK(st && elc_alert_decode(as_transfer(ELC_CH_ALERT, 0x01, *st), sv) && sv.active == 2 && sv.generation == 1);
  CHECK(play_is(last(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY), 4, 4, 0));
  const ElcAlertManager::Alert *t = r.m.top();
  CHECK(t && t->node == 0x01 && t->alert == 1);

  // STATE at 1 Hz, and at once on a change; an unchanged define is not re-sent.
  r.m.poll(500);
  s = r.bus.take();
  CHECK(count(s, ELC_CH_ALERT, ELC_ALERT_STATE) == 0);
  r.m.defineOwn(d);
  r.m.poll(1000);
  s = r.bus.take();
  CHECK(count(s, ELC_CH_ALERT, ELC_ALERT_STATE) == 1 && count(s, ELC_CH_ALERT, ELC_ALERT_DEFINE) == 0);
  r.m.setOwn(1, false);
  r.m.poll(1100);
  s = r.bus.take();
  st = last(s, ELC_CH_ALERT, ELC_ALERT_STATE);
  CHECK(st && elc_alert_decode(as_transfer(ELC_CH_ALERT, 0x01, *st), sv) && sv.active == 0);
  CHECK(count(s, ELC_CH_AUDIO_CMD, ELC_AUDIO_STOP) == 1 && r.m.ringing() == 0);

  // New text: re-sent with the generation bumped. QUERY bit2: every own DEFINE again.
  elc_ch_copy_text(d.text, sizeof(d.text), "Oil pressure 4 psi");
  r.m.defineOwn(d);
  r.m.poll(1200);
  s = r.bus.take();
  st = last(s, ELC_CH_ALERT, ELC_ALERT_STATE);
  CHECK(count(s, ELC_CH_ALERT, ELC_ALERT_DEFINE) == 1);
  CHECK(st && elc_alert_decode(as_transfer(ELC_CH_ALERT, 0x01, *st), sv) && sv.generation == 2);
  r.m.describeOwn();
  r.m.poll(1300);
  s = r.bus.take();
  CHECK(count(s, ELC_CH_ALERT, ELC_ALERT_DEFINE) == 1);
}

int main() {
  test_collect_and_chime();
  test_warning_rings_until_ack();
  test_order_latch_and_lost();
  test_own_alerts();
  if (g_failures) {
    std::printf("%d FAILED\n", g_failures);
    return 1;
  }
  std::printf("all alert manager tests passed\n");
  return 0;
}
