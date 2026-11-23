// Host tests for BULK and the firmware update profile (elc_bulk.cpp, elc_fw.cpp). Run: ./run_tests.sh
//
// A simulated bus: every node is an ElcChannels; frames a node sends are delivered to every other node on the next
// millisecond, at most kFramesPerMs of them (500 kbit/s carries about 3.7 frames a millisecond). A drop rule can lose
// data frames on their way to one receiver, as a receiver that falls behind its receive queue would.

#include <elc_bulk.h>
#include <elc_fw.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static const int kFramesPerMs = 3;

struct Bus;
struct Node {
  Bus *bus = nullptr;
  uint8_t id = 0;
  ElcChannels ch;
};

struct Bus {
  std::vector<Node *> nodes;
  std::deque<std::pair<uint8_t, ElcCanFrame>> q;  // sender, frame
  uint32_t now = 0;
  // drop(frame, to) -> true loses it on the way to that node
  bool (*drop)(const ElcCanFrame &f, uint8_t to, void *ctx) = nullptr;
  void *drop_ctx = nullptr;
  size_t tx_cap = 64;  // frames the bus queue holds (the writers' transmit queues together)
  uint32_t data_frames = 0;

  void add(Node &n, uint8_t id, const char *name) {
    n.bus = this;
    n.id = id;
    ElcNetAnnounce me;
    me.type = ELC_TYPE_DISPLAY;
    snprintf(me.name, sizeof(me.name), "%s", name);
    n.ch.begin(id, me, &Bus::send_fn, &n);
    nodes.push_back(&n);
  }
  static bool send_fn(const ElcCanFrame &f, void *ctx) {
    Node *n = (Node *)ctx;
    if (n->bus->q.size() >= n->bus->tx_cap) return false;
    n->bus->q.push_back({n->id, f});
    return true;
  }
  void tick() {
    now++;
    for (int k = 0; k < kFramesPerMs && !q.empty(); k++) {
      auto [src, f] = q.front();
      q.pop_front();
      if ((f.id & ~0x1F) == ELC_CH_BULK_DATA) data_frames++;
      for (Node *n : nodes) {
        if (n->id == src) continue;
        if (drop && drop(f, n->id, drop_ctx)) continue;
        n->ch.feed(f, now);
      }
    }
    for (Node *n : nodes) n->ch.poll(now);
  }
  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) tick();
  }
};

// The spare OTA slot, in memory.
struct MemFlash : ElcFwFlash {
  std::vector<uint8_t> img;
  uint32_t want = 0;
  bool open = false, committed = false, aborted = false;
  uint32_t cap = 1u << 20;
  uint32_t pending_until = 0;  // commit() is PENDING until the bus reaches this time
  Bus *bus = nullptr;
  uint32_t capacity() override { return cap; }
  bool begin(uint32_t n) override {
    img.clear();
    want = n;
    open = true;
    return true;
  }
  bool write(const uint8_t *p, size_t n) override {
    if (!open) return false;
    img.insert(img.end(), p, p + n);
    return img.size() <= want;
  }
  uint8_t commit() override { return committing(); }
  uint8_t committing() override {
    if (bus && bus->now < pending_until) return ELC_BULK_PENDING;
    committed = img.size() == want;
    open = false;
    return committed ? ELC_BULK_OK : ELC_BULK_REJECTED;
  }
  void abort() override {
    open = false;
    aborted = true;
  }
};

// A firmware file: header + image bytes.
static std::vector<uint8_t> make_file(uint16_t product, const char *variant, uint32_t size, ElcFwHeader *hdr_out,
                                      uint32_t seed = 1) {
  std::vector<uint8_t> img(size);
  srand(seed);
  for (auto &b : img) b = (uint8_t)rand();
  ElcFwHeader h;
  h.product = product;
  h.fw = 7;
  h.image_size = size;
  h.image_crc = elc_crc32(img.data(), img.size());
  h.build_time = 1790000000u;
  snprintf(h.variant, sizeof(h.variant), "%s", variant);
  uint8_t head[ELC_FW_HEADER_SIZE];
  elc_fw_header_encode(h, head);
  std::vector<uint8_t> file(head, head + ELC_FW_HEADER_SIZE);
  file.insert(file.end(), img.begin(), img.end());
  if (hdr_out) *hdr_out = h;
  return file;
}

struct FileSource {
  const std::vector<uint8_t> *file = nullptr;
  uint32_t available = 0xFFFFFFFFu;  // a streaming source has only this many bytes so far
  uint32_t lowest_read = 0xFFFFFFFFu;
  const ElcBulkWriter *w = nullptr;
  bool read_below_low_water = false;
  static size_t read(uint32_t off, uint8_t *buf, size_t n, void *ctx) {
    FileSource *s = (FileSource *)ctx;
    if (s->w && off < s->w->lowWater() * ELC_BULK_CHUNK_BYTES) s->read_below_low_water = true;
    if (off + n > s->available) return 0;
    if (off >= s->file->size()) return 0;
    if (off + n > s->file->size()) n = s->file->size() - off;
    memcpy(buf, s->file->data() + off, n);
    if (off < s->lowest_read) s->lowest_read = off;
    return n;
  }
};

static bool raw_send(const ElcCanFrame &f, void *ctx) { return ((ElcChannels *)ctx)->sendRaw(f); }

// A small cluster: the console writes, three screens take (center built for the 3.4", left and right for the 4").
struct Cluster {
  Bus bus;
  Node console, center, left, right;
  ElcBulkWriter writer;
  MemFlash fc, fl, fr;
  ElcFwTarget tc, tl, tr;
  Cluster() {
    bus.add(console, 0x13, "Console");
    bus.add(center, 0x01, "Center");
    bus.add(left, 0x02, "Left");
    bus.add(right, 0x03, "Right");
    writer.begin(&console.ch, raw_send, &console.ch);
    fc.bus = fl.bus = fr.bus = &bus;
    tc.begin(&center.ch, &fc, ELC_PRODUCT_DISPLAY, "display-waveshare3c4", 6);
    tl.begin(&left.ch, &fl, ELC_PRODUCT_DISPLAY, "display-waveshare4c", 6);
    tr.begin(&right.ch, &fr, ELC_PRODUCT_DISPLAY, "display-waveshare4c", 6);
    bus.run(50);  // boot ANNOUNCEs
  }
  bool run_until_done(uint32_t max_ms) {
    for (uint32_t i = 0; i < max_ms; i++) {
      bus.tick();
      if (!writer.busy()) return true;
    }
    return false;
  }
};

static const ElcBulkWriter::Receiver *rx_of(const ElcBulkWriter &w, uint8_t node) {
  for (int i = 0; i < w.receivers(); i++) {
    if (w.receiver(i).node == node) return &w.receiver(i);
  }
  return nullptr;
}

static void test_codecs() {
  std::printf("crc-32, header, messages\n");
  CHECK(elc_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
  // in pieces, the same
  CHECK(elc_crc32((const uint8_t *)"6789", 4, elc_crc32((const uint8_t *)"12345", 5)) == 0xCBF43926u);

  ElcFwHeader h, back;
  h.product = 3;
  h.fw = 12;
  h.image_size = 123456;
  h.image_crc = 0xDEADBEEF;
  h.build_time = 1790000000u;
  h.commit = 0x16b8cea0;
  snprintf(h.variant, sizeof(h.variant), "console");
  uint8_t b[ELC_FW_HEADER_SIZE];
  CHECK(elc_fw_header_encode(h, b) == 64);
  CHECK(memcmp(b, "ELFW", 4) == 0 && b[4] == 1 && b[5] == 64);
  CHECK(elc_fw_header_decode(b, sizeof(b), back));
  CHECK(back.product == 3 && back.fw == 12 && back.image_size == 123456 && back.image_crc == 0xDEADBEEF &&
        back.commit == 0x16b8cea0 && strcmp(back.variant, "console") == 0);
  b[30] ^= 1;  // a flipped bit in the variant
  CHECK(!elc_fw_header_decode(b, sizeof(b), back));
  CHECK(!elc_fw_header_decode(b, 63, back));

  ElcBulkWrite w = elc_fw_offer(h, 0x13), w2;
  w.tag = 9;
  w.session = 5;
  uint8_t m[64];
  const size_t n = elc_bulk_encode(w, m, sizeof(m));
  CHECK(n == 1 + 1 + 1 + 1 + 2 + 4 + 4 + 1 + 2 + 7);
  CHECK(elc_bulk_decode(m, n, w2));
  CHECK(w2.dest == 0x13 && w2.tag == 9 && w2.kind == 2 && w2.object == 3 && w2.size == 123456 + 64 &&
        w2.crc == 0xDEADBEEF && w2.session == 5 && w2.fw == 12 && strcmp(w2.variant, "console") == 0);

  ElcBulkWant want, want2;
  want.dest = 0x13;
  want.session = 5;
  want.first = 0x123456;
  want.credit = 96;
  CHECK(elc_bulk_encode(want, m, sizeof(m)) == 7);
  CHECK(elc_bulk_decode(m, 7, want2) && want2.first == 0x123456 && want2.credit == 96 && want2.session == 5);

  ElcBulkInfo info, info2;
  info.to = 0x13;
  info.tag = 9;
  info.session = ELC_BULK_REFUSED;
  info.why = ELC_BULK_WHY_TARGET;
  const size_t in = elc_bulk_encode(info, m, sizeof(m));
  CHECK(in == 13 && elc_bulk_decode(m, in, info2) && info2.session == ELC_BULK_REFUSED && info2.why == ELC_BULK_WHY_TARGET);

  ElcCanFrame f;
  const uint8_t five[5] = {1, 2, 3, 4, 5};
  elc_bulk_data_frame(0x13, 5, 0x1ABCDE, five, 5, f);
  uint8_t s;
  uint32_t c;
  const uint8_t *p;
  size_t pn;
  CHECK(f.id == 0x353 && f.len == 8);
  CHECK(elc_bulk_data_decode(f, &s, &c, &p, &pn) && s == 5 && c == 0x1ABCDE && pn == 5 && p[4] == 5);
}

static void test_multicast() {
  std::printf("multicast: one 4\" build to both 4\" screens, the 3.4\" center says no\n");
  Cluster cl;
  ElcFwHeader h;
  const auto file = make_file(ELC_PRODUCT_DISPLAY, "display-waveshare4c", 20000, &h);
  FileSource src;
  src.file = &file;
  src.w = &cl.writer;
  CHECK(cl.writer.start(elc_fw_offer(h), FileSource::read, &src, cl.bus.now));
  CHECK(cl.run_until_done(20000));
  CHECK(cl.writer.succeeded());
  CHECK(cl.writer.progress() == 1000);
  const auto *rl = rx_of(cl.writer, 0x02), *rr = rx_of(cl.writer, 0x03), *rc = rx_of(cl.writer, 0x01);
  CHECK(rl && rl->state == ElcBulkWriter::RxState::Done);
  CHECK(rr && rr->state == ElcBulkWriter::RxState::Done);
  CHECK(rc && rc->state == ElcBulkWriter::RxState::Refused && rc->result == ELC_BULK_WHY_TARGET);
  CHECK(cl.fl.committed && cl.fr.committed && !cl.fc.open && cl.fc.img.empty());
  CHECK(cl.fl.img.size() == 20000 && memcmp(cl.fl.img.data(), file.data() + 64, 20000) == 0);
  CHECK(cl.fr.img == cl.fl.img);
  CHECK(cl.tl.phase() == ElcFwTarget::Phase::Done && cl.tc.phase() == ElcFwTarget::Phase::Idle);
  // one frame feeds both receivers: about one pass over the object, not two
  const uint32_t chunks = elc_bulk_chunks((uint32_t)file.size());
  CHECK(cl.writer.framesSent() >= chunks && cl.writer.framesSent() < chunks + chunks / 20);
  std::printf("  %u chunks, %u frames sent, %u resends, %u ms\n", (unsigned)chunks, (unsigned)cl.writer.framesSent(),
              (unsigned)cl.writer.resends(), (unsigned)cl.bus.now);
  // the targets restart once, after the END repeats
  CHECK(!cl.tl.restartDue(cl.bus.now));
  cl.bus.run(ElcFwTarget::kRestartMs + 10);
  CHECK(cl.tl.restartDue(cl.bus.now) && !cl.tl.restartDue(cl.bus.now));
  CHECK(cl.tr.restartDue(cl.bus.now) && !cl.tc.restartDue(cl.bus.now));
}

struct LossRule {
  uint32_t n = 0;
  uint8_t to = 0;   // 0 = every receiver
  uint32_t every = 50;
};
static bool lossy(const ElcCanFrame &f, uint8_t to, void *ctx) {
  LossRule *r = (LossRule *)ctx;
  if ((f.id & ~0x1F) != ELC_CH_BULK_DATA || (r->to && to != r->to)) return false;
  return (++r->n % r->every) == 0;
}

static void test_loss() {
  std::printf("lost data frames: re-asked, the image still arrives whole\n");
  Cluster cl;
  LossRule rule;
  rule.to = 0x03;  // only the right screen misses frames (its queue overflows, say)
  rule.every = 37;
  cl.bus.drop = lossy;
  cl.bus.drop_ctx = &rule;
  ElcFwHeader h;
  const auto file = make_file(ELC_PRODUCT_DISPLAY, "display-waveshare4c", 30001, &h, 7);  // not a multiple of 5
  FileSource src;
  src.file = &file;
  CHECK(cl.writer.start(elc_fw_offer(h), FileSource::read, &src, cl.bus.now));
  CHECK(cl.run_until_done(60000));
  CHECK(cl.writer.succeeded());
  CHECK(cl.fl.committed && cl.fr.committed);
  CHECK(cl.fr.img.size() == 30001 && memcmp(cl.fr.img.data(), file.data() + 64, 30001) == 0);
  CHECK(cl.fl.img == cl.fr.img);
  CHECK(cl.writer.resends() > 0);
  std::printf("  %u frames sent, %u resends, %u ms\n", (unsigned)cl.writer.framesSent(), (unsigned)cl.writer.resends(),
              (unsigned)cl.bus.now);

  // every receiver losing one frame in 20, and the WRITE's first INFO lost too
  Cluster c2;
  LossRule r2;
  r2.every = 20;
  c2.bus.drop = lossy;
  c2.bus.drop_ctx = &r2;
  CHECK(c2.writer.start(elc_fw_offer(h), FileSource::read, &src, c2.bus.now));
  CHECK(c2.run_until_done(60000));
  CHECK(c2.writer.succeeded() && c2.fl.committed && c2.fr.committed && c2.fl.img == cl.fl.img);
}

static void test_unicast_and_refusals() {
  std::printf("one node, bad CRC, wrong build in the header, not now, too big\n");
  {
    Cluster cl;
    ElcFwHeader h;
    const auto file = make_file(ELC_PRODUCT_DISPLAY, "display-waveshare3c4", 5000, &h);
    FileSource src;
    src.file = &file;
    CHECK(cl.writer.start(elc_fw_offer(h, 0x01), FileSource::read, &src, cl.bus.now));
    CHECK(cl.run_until_done(10000));
    CHECK(cl.writer.succeeded() && cl.writer.receivers() == 1 && cl.fc.committed);
    CHECK(cl.fl.img.empty() && cl.fr.img.empty());
  }
  {
    Cluster cl;
    ElcFwHeader h;
    const auto file = make_file(ELC_PRODUCT_DISPLAY, "display-waveshare4c", 5000, &h);
    ElcBulkWrite w = elc_fw_offer(h, 0x02);
    w.crc ^= 1;  // the offer says another CRC: the header no longer matches it
    FileSource src;
    src.file = &file;
    CHECK(cl.writer.start(w, FileSource::read, &src, cl.bus.now));
    CHECK(cl.run_until_done(10000));
    CHECK(!cl.writer.succeeded());
    CHECK(rx_of(cl.writer, 0x02) && rx_of(cl.writer, 0x02)->result == ELC_BULK_REJECTED);
    CHECK(!cl.fl.open && !cl.fl.committed && cl.fl.img.empty());  // flash never opened
  }
  {
    // the image itself corrupted on the way (the header is fine): CRC mismatch at the end
    Cluster cl;
    ElcFwHeader h;
    auto file = make_file(ELC_PRODUCT_DISPLAY, "display-waveshare4c", 5000, &h);
    file[3000] ^= 0x40;
    FileSource src;
    src.file = &file;
    CHECK(cl.writer.start(elc_fw_offer(h, 0x02), FileSource::read, &src, cl.bus.now));
    CHECK(cl.run_until_done(10000));
    CHECK(!cl.writer.succeeded() && rx_of(cl.writer, 0x02)->result == ELC_BULK_CRC);
    CHECK(cl.fl.aborted && !cl.fl.committed);
    CHECK(cl.tl.phase() == ElcFwTarget::Phase::Failed);
    cl.bus.run(3000);
    CHECK(!cl.tl.restartDue(cl.bus.now));
  }
  {
    // a 3.4" file offered as if it were the 4" build: the header gives it away before flash is touched
    Cluster cl;
    ElcFwHeader h;
    const auto file = make_file(ELC_PRODUCT_DISPLAY, "display-waveshare3c4", 5000, &h);
    ElcBulkWrite w = elc_fw_offer(h, 0);
    snprintf(w.variant, sizeof(w.variant), "display-waveshare4c");
    FileSource src;
    src.file = &file;
    CHECK(cl.writer.start(w, FileSource::read, &src, cl.bus.now));
    CHECK(cl.run_until_done(10000));
    CHECK(!cl.writer.succeeded());
    CHECK(rx_of(cl.writer, 0x02)->result == ELC_BULK_REJECTED && rx_of(cl.writer, 0x03)->result == ELC_BULK_REJECTED);
    CHECK(cl.fl.img.empty() && !cl.fl.committed && cl.fc.img.empty());
  }
  {
    Cluster cl;
    static bool moving = true;
    cl.tl.setAllowed([](void *) { return !moving; }, nullptr);
    ElcFwHeader h;
    const auto file = make_file(ELC_PRODUCT_DISPLAY, "display-waveshare4c", 5000, &h);
    FileSource src;
    src.file = &file;
    CHECK(cl.writer.start(elc_fw_offer(h, 0x02), FileSource::read, &src, cl.bus.now));
    CHECK(cl.run_until_done(5000));
    CHECK(!cl.writer.succeeded() && rx_of(cl.writer, 0x02)->state == ElcBulkWriter::RxState::Refused &&
          rx_of(cl.writer, 0x02)->result == ELC_BULK_WHY_NOT_NOW);
    CHECK(cl.bus.now < 1000);  // a refusal from the one node asked ends it at once
    moving = false;
    CHECK(cl.writer.start(elc_fw_offer(h, 0x02), FileSource::read, &src, cl.bus.now));
    CHECK(cl.run_until_done(10000) && cl.writer.succeeded());
  }
  {
    Cluster cl;
    cl.fl.cap = 4000;
    ElcFwHeader h;
    const auto file = make_file(ELC_PRODUCT_DISPLAY, "display-waveshare4c", 5000, &h);
    FileSource src;
    src.file = &file;
    CHECK(cl.writer.start(elc_fw_offer(h, 0x02), FileSource::read, &src, cl.bus.now));
    CHECK(cl.run_until_done(5000));
    CHECK(rx_of(cl.writer, 0x02)->result == ELC_BULK_WHY_SIZE);
  }
  {
    // nobody of that product on the bus: three WRITEs, then done with no receivers
    Cluster cl;
    ElcFwHeader h;
    const auto file = make_file(ELC_PRODUCT_RADIO, "radio", 500, &h);
    FileSource src;
    src.file = &file;
    CHECK(cl.writer.start(elc_fw_offer(h), FileSource::read, &src, cl.bus.now));
    CHECK(cl.run_until_done(10000));
    CHECK(!cl.writer.succeeded() && cl.writer.receivers() == 0);
  }
}

static void test_streaming_and_slow_commit() {
  std::printf("streaming source (the console's upload), a slow flash commit, abort\n");
  Cluster cl;
  ElcFwHeader h;
  const auto file = make_file(ELC_PRODUCT_DISPLAY, "display-waveshare4c", 40000, &h, 3);
  FileSource src;
  src.file = &file;
  src.w = &cl.writer;
  src.available = 64;  // only the header so far
  // the screens take 8 s to write flash after the last byte: longer than the writer's silence limit
  cl.fl.pending_until = cl.fr.pending_until = 0xFFFFFFFFu;
  CHECK(cl.writer.start(elc_fw_offer(h), FileSource::read, &src, cl.bus.now));
  uint32_t done_at = 0;
  for (uint32_t i = 0; i < 60000 && cl.writer.busy(); i++) {
    // the upload trickles in, held back to 32 KB ahead of what everyone has (the console's ring buffer)
    const uint32_t low = cl.writer.lowWater() * ELC_BULK_CHUNK_BYTES;
    if (src.available < file.size() && src.available < low + 32768) src.available += 40;
    if (src.available > file.size()) src.available = (uint32_t)file.size();
    if (!done_at && cl.tl.phase() == ElcFwTarget::Phase::Writing && cl.tr.phase() == ElcFwTarget::Phase::Writing) {
      done_at = cl.bus.now;
      cl.fl.pending_until = cl.fr.pending_until = done_at + 8000;
    }
    cl.bus.tick();
  }
  CHECK(done_at != 0);
  CHECK(cl.writer.succeeded());
  CHECK(cl.bus.now >= done_at + 8000);
  CHECK(!src.read_below_low_water);
  CHECK(cl.fl.committed && cl.fr.committed && cl.fl.img.size() == 40000 &&
        memcmp(cl.fl.img.data(), file.data() + 64, 40000) == 0);

  // abort halfway: both screens stop and close their slot
  Cluster c2;
  FileSource s2;
  s2.file = &file;
  CHECK(c2.writer.start(elc_fw_offer(h), FileSource::read, &s2, c2.bus.now));
  for (int i = 0; i < 20000 && c2.writer.progress() < 400; i++) c2.bus.tick();
  CHECK(c2.tl.active() && c2.tr.active());
  c2.writer.abort(c2.bus.now);
  c2.bus.run(100);
  CHECK(!c2.writer.busy() && !c2.writer.succeeded());
  CHECK(c2.tl.phase() == ElcFwTarget::Phase::Failed && c2.fl.aborted && c2.fr.aborted && !c2.fl.committed);

  // the writer vanishes (the console lost power): the screens give up on their own
  Cluster c3;
  FileSource s3;
  s3.file = &file;
  CHECK(c3.writer.start(elc_fw_offer(h), FileSource::read, &s3, c3.bus.now));
  for (int i = 0; i < 20000 && c3.writer.progress() < 300; i++) c3.bus.tick();
  c3.bus.nodes.erase(c3.bus.nodes.begin());  // the console's frames stop
  c3.bus.q.clear();
  c3.bus.run(ElcBulkReceiver::kGiveUpMs + 500);
  CHECK(c3.tl.phase() == ElcFwTarget::Phase::Failed && c3.fl.aborted && !c3.tl.active());
}

static void test_busy() {
  std::printf("a second writer while one is in progress is told busy\n");
  Cluster cl;
  Node tool;
  cl.bus.add(tool, 0x1F, "Service tool");
  ElcBulkWriter w2;
  w2.begin(&tool.ch, raw_send, &tool.ch);
  ElcFwHeader h;
  const auto file = make_file(ELC_PRODUCT_DISPLAY, "display-waveshare4c", 20000, &h);
  FileSource src;
  src.file = &file;
  CHECK(cl.writer.start(elc_fw_offer(h, 0x02), FileSource::read, &src, cl.bus.now));
  cl.bus.run(200);
  CHECK(w2.start(elc_fw_offer(h, 0x02), FileSource::read, &src, cl.bus.now));
  for (int i = 0; i < 3000 && w2.busy(); i++) cl.bus.tick();
  CHECK(!w2.busy() && rx_of(w2, 0x02) && rx_of(w2, 0x02)->result == ELC_BULK_WHY_BUSY);
  CHECK(cl.run_until_done(20000) && cl.writer.succeeded());
}

int main() {
  test_codecs();
  test_multicast();
  test_loss();
  test_unicast_and_refusals();
  test_streaming_and_slow_commit();
  test_busy();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("all bulk tests passed\n");
  return 0;
}
