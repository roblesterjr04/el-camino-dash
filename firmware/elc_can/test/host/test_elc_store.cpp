// Host tests for the STORE channel (elc_can/src/elc_store.cpp): a client node talks to the store over a fake bus,
// with the records kept in a map. Run: ./run_tests.sh

#include <elc_chan.h>
#include <elc_chan_msg.h>
#include <elc_store.h>

#include <cstdio>
#include <cstring>
#include <map>
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

struct Bus {
  std::vector<ElcCanFrame> frames;
};
static bool bus_send(const ElcCanFrame &f, void *ctx) {
  ((Bus *)ctx)->frames.push_back(f);
  return true;
}

class MapBackend : public ElcStoreBackend {
 public:
  std::map<std::string, std::string> files;  // "owner/key" -> bytes
  bool media = true;
  bool fail_writes = false;
  bool ready() override { return media; }
  int32_t size(const char *owner, const char *key) override {
    auto it = files.find(path(owner, key));
    return it == files.end() ? -1 : (int32_t)it->second.size();
  }
  int count(const char *owner) override {
    const std::string pre = std::string(owner) + "/";
    int n = 0;
    for (auto &kv : files) n += kv.first.compare(0, pre.size(), pre) == 0;
    return n;
  }
  bool write(const char *owner, const char *key, const uint8_t *p, size_t n, bool append) override {
    if (fail_writes) return false;
    std::string &f = files[path(owner, key)];
    if (!append) f.clear();
    f.append((const char *)p, n);
    return true;
  }
  int32_t read(const char *owner, const char *key, uint32_t offset, uint8_t *out, size_t cap) override {
    auto it = files.find(path(owner, key));
    if (it == files.end() || offset > it->second.size()) return -1;
    const size_t n = std::min(cap, it->second.size() - offset);
    memcpy(out, it->second.data() + offset, n);
    return (int32_t)n;
  }
  bool remove(const char *owner, const char *key) override { return files.erase(path(owner, key)) == 1; }

 private:
  static std::string path(const char *o, const char *k) { return std::string(o) + "/" + k; }
};

// The center (store) and one accessory, each with its own bus capture; deliver() moves frames across.
struct Rig {
  Bus cbus, abus;
  ElcChannels center, acc;
  MapBackend backend;
  ElcStoreServer store;
  std::vector<std::vector<uint8_t>> to_acc;  // complete transfers the accessory heard on STORE
  uint32_t now = 0;

  Rig(uint8_t acc_node = 0x14, bool announce = true) {
    ElcNetAnnounce c;
    c.type = ELC_TYPE_DISPLAY;
    c.product = ELC_PRODUCT_DISPLAY;
    c.consumes = elc_ch_bit(ELC_CH_STORE);
    center.begin(0x01, c, bus_send, &cbus);
    center.onTransfer([](const ElcChTransfer &t, void *ctx) { ((Rig *)ctx)->store.handle(t); }, this);
    store.begin(&center, &backend);
    ElcNetAnnounce a;
    a.type = ELC_TYPE_SENSOR;
    a.product = ELC_PRODUCT_GPS;
    std::strcpy(a.name, "GPS");
    acc.begin(acc_node, a, bus_send, &abus);
    acc.onTransfer(
        [](const ElcChTransfer &t, void *ctx) {
          if (t.base == ELC_CH_STORE) ((Rig *)ctx)->to_acc.emplace_back(t.data, t.data + t.len);
        },
        this);
    center.poll(0);
    if (announce) acc.poll(0);
    else acc.poll(0), abus.frames.clear();  // its boot ANNOUNCE is lost
    deliver();
    to_acc.clear();
  }
  void deliver() {
    now += 1;
    for (const ElcCanFrame &f : abus.frames) center.feed(f, now);
    abus.frames.clear();
    for (const ElcCanFrame &f : cbus.frames) acc.feed(f, now);
    cbus.frames.clear();
  }
  // One request from the accessory, delivered, and the store's answer.
  std::vector<uint8_t> ask(const ElcChWriter &w) {
    to_acc.clear();
    acc.send(ELC_CH_STORE, w);
    deliver();
    deliver();
    return to_acc.empty() ? std::vector<uint8_t>() : to_acc.back();
  }
  ElcStoreResult put(const char *key, const void *p, size_t n, uint8_t mode = ELC_STORE_REPLACE, uint8_t tag = 1) {
    ElcStorePut v;
    v.dest = 0x01;
    v.tag = tag;
    v.mode = mode;
    std::strcpy(v.key, key);
    v.data = (const uint8_t *)p;
    v.len = n;
    ElcChWriter w;
    elc_store_encode(v, w);
    return result(ask(w));
  }
  ElcStoreResult del(const char *key, uint8_t tag = 2) {
    ElcStoreDelete v;
    v.dest = 0x01;
    v.tag = tag;
    std::strcpy(v.key, key);
    ElcChWriter w;
    elc_store_encode(v, w);
    return result(ask(w));
  }
  std::vector<uint8_t> last;  // the last GET's answer: DATA points into it
  const std::vector<uint8_t> &get_raw(const char *key, uint32_t offset = 0, uint8_t max = 0, uint8_t tag = 3) {
    ElcStoreGet v;
    v.dest = 0x01;
    v.tag = tag;
    v.offset = offset;
    v.max = max;
    std::strcpy(v.key, key);
    ElcChWriter w;
    elc_store_encode(v, w);
    last = ask(w);
    return last;
  }
  static ElcStoreResult result(const std::vector<uint8_t> &p) {
    ElcStoreResult r;
    r.result = 0xEE;
    ElcChTransfer t;
    t.base = ELC_CH_STORE;
    t.data = p.data();
    t.len = p.size();
    if (!elc_store_decode(t, r)) r.result = 0xEE;
    return r;
  }
};

static bool decode_data(const std::vector<uint8_t> &p, ElcStoreData &d) {
  ElcChTransfer t;
  t.base = ELC_CH_STORE;
  t.data = p.data();
  t.len = p.size();
  return elc_store_decode(t, d);
}

static void test_codecs() {
  std::printf("store codecs\n");
  CHECK(elc_ch_base(0x294) == ELC_CH_STORE && elc_ch_base(0x281) == ELC_CH_STORE);
  CHECK(elc_store_key_ok("trip.log") && elc_store_key_ok("A-b_9") && elc_store_key_ok("abcdefghijklmnopqrstuvwx"));
  CHECK(!elc_store_key_ok("") && !elc_store_key_ok(".hidden") && !elc_store_key_ok("a/b") &&
        !elc_store_key_ok("a b") && !elc_store_key_ok("abcdefghijklmnopqrstuvwxy") && !elc_store_key_ok(nullptr));

  const uint8_t bytes[3] = {9, 8, 7};
  ElcStorePut p;
  p.dest = 1;
  p.tag = 42;
  p.mode = ELC_STORE_APPEND;
  std::strcpy(p.key, "trip");
  p.data = bytes;
  p.len = 3;
  ElcChWriter w;
  elc_store_encode(p, w);
  const uint8_t want[] = {0x81, 1, 42, 1, 4, 't', 'r', 'i', 'p', 9, 8, 7};
  CHECK(w.size() == sizeof(want) && memcmp(w.data(), want, sizeof(want)) == 0);
  ElcChTransfer t;
  t.base = ELC_CH_STORE;
  t.src = 0x14;
  t.data = w.data();
  t.len = w.size();
  ElcStorePut q;
  CHECK(elc_store_decode(t, q) && q.tag == 42 && q.mode == 1 && std::strcmp(q.key, "trip") == 0 && q.len == 3 &&
        memcmp(q.data, bytes, 3) == 0);
  CHECK(t.for_me(0x01) && !t.for_me(0x02));

  // A key length past the payload is malformed; one over 24 bytes decodes as "" (bad key, not a cut name).
  const uint8_t bad[] = {0x81, 1, 1, 0, 9, 'a', 'b'};
  t.data = bad;
  t.len = sizeof(bad);
  CHECK(!elc_store_decode(t, q));
  uint8_t longkey[5 + 30] = {0x81, 1, 1, 0, 30};
  memset(longkey + 5, 'k', 30);
  t.data = longkey;
  t.len = sizeof(longkey);
  CHECK(elc_store_decode(t, q) && q.key[0] == 0 && q.len == 0);
}

static void test_put_get_delete() {
  std::printf("store put / get / delete\n");
  Rig r;
  CHECK(r.center.nodeInfo(0x14).announced);

  ElcStoreResult res = r.put("trip", "hello", 5, ELC_STORE_REPLACE, 7);
  CHECK(res.result == ELC_STORE_OK && res.dest == 0x14 && res.tag == 7 && res.size == 5);
  CHECK(r.backend.files.count("0005_14/trip") && r.backend.files["0005_14/trip"] == "hello");

  res = r.put("trip", " world", 6, ELC_STORE_APPEND);
  CHECK(res.result == ELC_STORE_OK && res.size == 11 && r.backend.files["0005_14/trip"] == "hello world");

  ElcStoreData d;
  CHECK(decode_data(r.get_raw("trip", 0, 0, 9), d) && d.result == ELC_STORE_OK && d.dest == 0x14 && d.tag == 9);
  CHECK(d.size == 11 && d.offset == 0 && d.len == 11 && memcmp(d.data, "hello world", 11) == 0);
  CHECK(decode_data(r.get_raw("trip", 6, 3), d) && d.result == ELC_STORE_OK && d.len == 3 &&
        memcmp(d.data, "wor", 3) == 0);
  CHECK(decode_data(r.get_raw("trip", 11), d) && d.result == ELC_STORE_OK && d.len == 0 && d.size == 11);  // the end
  CHECK(decode_data(r.get_raw("trip", 12), d) && d.result == ELC_STORE_BAD);
  CHECK(decode_data(r.get_raw("nope"), d) && d.result == ELC_STORE_NOT_FOUND && d.len == 0);

  // Replace shrinks it; delete removes it.
  res = r.put("trip", "x", 1);
  CHECK(res.result == ELC_STORE_OK && res.size == 1 && r.backend.files["0005_14/trip"] == "x");
  CHECK(r.del("trip").result == ELC_STORE_OK && !r.backend.files.count("0005_14/trip"));
  CHECK(r.del("trip").result == ELC_STORE_NOT_FOUND);

  // A large record comes back in pieces of at most 244 bytes (multi-frame transfers both ways).
  std::string big(600, 0);
  for (size_t i = 0; i < big.size(); i++) big[i] = (char)(i * 13);
  r.backend.files["0005_14/big"] = big;
  std::string back;
  for (uint32_t off = 0; off < big.size();) {
    CHECK(decode_data(r.get_raw("big", off), d) && d.result == ELC_STORE_OK && d.len > 0 && d.len <= ELC_STORE_DATA_MAX);
    if (d.result != ELC_STORE_OK || !d.len) break;
    back.append((const char *)d.data, d.len);
    off += (uint32_t)d.len;
  }
  CHECK(back == big);
  // A full-size PUT (256-byte transfer).
  std::string chunk(256 - 5 - 3, 'z');
  CHECK(r.put("max", chunk.data(), chunk.size()).result == ELC_STORE_OK && r.backend.files["0005_14/max"] == chunk);

  CHECK(r.store.stats().puts == 4 && r.store.stats().deletes == 2);
}

static void test_refusals() {
  std::printf("store refusals\n");
  {
    Rig r;
    CHECK(r.put(".x", "a", 1).result == ELC_STORE_BAD_KEY);
    CHECK(r.put("a/b", "a", 1).result == ELC_STORE_BAD_KEY && r.backend.files.empty());
    CHECK(r.put("k", "a", 1, 5).result == ELC_STORE_BAD);  // unknown mode

    r.backend.media = false;
    CHECK(r.put("k", "a", 1).result == ELC_STORE_NO_MEDIA);
    ElcStoreData d;
    CHECK(decode_data(r.get_raw("k"), d) && d.result == ELC_STORE_NO_MEDIA);
    r.backend.media = true;

    r.backend.fail_writes = true;
    CHECK(r.put("k", "a", 1).result == ELC_STORE_IO);
    r.backend.fail_writes = false;

    // A record may not grow past 64 KB.
    r.backend.files["0005_14/full"] = std::string(ELC_STORE_RECORD_MAX - 2, 'a');
    CHECK(r.put("full", "ab", 2, ELC_STORE_APPEND).result == ELC_STORE_OK);
    ElcStoreResult res = r.put("full", "c", 1, ELC_STORE_APPEND);
    CHECK(res.result == ELC_STORE_FULL && res.size == ELC_STORE_RECORD_MAX);

    // At most 64 records per node; replacing one that exists still works.
    r.backend.files.clear();
    for (int i = 0; i < ELC_STORE_RECORDS_MAX; i++) r.backend.files["0005_14/r" + std::to_string(i)] = "x";
    CHECK(r.put("one-more", "a", 1).result == ELC_STORE_FULL);
    CHECK(r.put("r3", "new", 3).result == ELC_STORE_OK);
    CHECK(r.store.stats().refused >= 7);
  }
  {
    // A node the store has not heard announce: BUSY, and the store asks it to announce. After that it works.
    Rig r(0x15, false);
    CHECK(!r.center.nodeInfo(0x15).announced);
    CHECK(r.put("k", "a", 1).result == ELC_STORE_BUSY && r.backend.files.empty());
    r.acc.poll(300);  // answers the QUERY after (node x 8) ms
    r.deliver();
    CHECK(r.center.nodeInfo(0x15).announced);
    CHECK(r.put("k", "a", 1).result == ELC_STORE_OK && r.backend.files.count("0005_15/k"));
  }
  {
    // Addressed to another node, or a broadcast opcode: not the store's.
    Rig r;
    ElcStorePut v;
    v.dest = 0x02;
    std::strcpy(v.key, "k");
    ElcChWriter w;
    elc_store_encode(v, w);
    CHECK(r.ask(w).empty() && r.backend.files.empty());
    ElcStoreResult fake;
    fake.dest = 0x01;
    ElcChWriter w2;
    elc_store_encode(fake, w2);
    CHECK(r.ask(w2).empty());
    // dest 0x00 (every node) reaches the store.
    v.dest = 0x00;
    ElcChWriter w3;
    elc_store_encode(v, w3);
    CHECK(Rig::result(r.ask(w3)).result == ELC_STORE_OK && r.backend.files.count("0005_14/k"));
  }
}

int main() {
  test_codecs();
  test_put_get_delete();
  test_refusals();
  if (g_failures) {
    std::printf("%d FAILED\n", g_failures);
    return 1;
  }
  std::printf("all store tests passed\n");
  return 0;
}
