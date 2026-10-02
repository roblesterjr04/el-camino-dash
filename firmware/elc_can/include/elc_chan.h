#pragma once

// The channel protocol (docs/protocols/can_channels.md): transfers framed with the tail byte (single frame, or
// multi-frame with a CRC-16/CCITT-FALSE), reassembled per CAN ID, and NET discovery (ANNOUNCE / QUERY / IDENTIFY /
// RESTART / RAIL). No Arduino or ESP-IDF dependencies, so it is host-tested (test/host/run_tests.sh).
//
// A board sets up one ElcChannels with its ANNOUNCE and hands it to ElcCan::attachChannels(): every received frame
// then goes through it, complete transfers reach onTransfer(), and the heartbeat says "speaks the channel protocol".

#include <stddef.h>
#include <stdint.h>

#include "elc_can_codec.h"

// ---- channels: base + node (can_channels.md, ID map) ----
static const uint16_t ELC_CH_ALERT = 0x020;
static const uint16_t ELC_CH_INPUT = 0x040;
static const uint16_t ELC_CH_SYSTEM = 0x060;
static const uint16_t ELC_CH_SENSOR = 0x120;
static const uint16_t ELC_CH_AUDIO_CMD = 0x240;
static const uint16_t ELC_CH_UI = 0x260;
static const uint16_t ELC_CH_AUDIO_STREAM = 0x300;  // raw (no tail byte)
static const uint16_t ELC_CH_BULK = 0x320;
static const uint16_t ELC_CH_BULK_DATA = 0x340;     // raw
static const uint16_t ELC_CH_ONLINE = 0x360;
static const uint16_t ELC_CH_NET = 0x3C0;
static const uint16_t ELC_CH_LOG = 0x3E0;

/** Bit of a channel in ANNOUNCE's produces / consumes maps (channel number = base >> 5). */
static inline uint32_t elc_ch_bit(uint16_t base) { return 1u << (base >> 5); }
/** The channel base of a CAN ID that carries transfers, or 0 (legacy, raw, private range, heartbeat). */
uint16_t elc_ch_base(uint16_t id);
static inline uint8_t elc_ch_src(uint16_t id) { return (uint8_t)(id & 0x1F); }

static const size_t ELC_CH_MAX_PAYLOAD = 256;
static const size_t ELC_CH_MAX_FRAMES = (ELC_CH_MAX_PAYLOAD + 2 + 6) / 7;  // 38

uint16_t elc_ch_crc16(const uint8_t *p, size_t n);

/** Frames of one transfer on CAN ID `id` (base + sender). Returns the number written to out (0 = bad length or
 *  `max` too small). */
size_t elc_ch_frames(uint16_t id, const uint8_t *payload, size_t len, uint8_t tid, ElcCanFrame *out, size_t max);

/** A complete transfer: `data` stays valid until the next feed(). */
struct ElcChTransfer {
  uint16_t base = 0;
  uint8_t src = 0;
  const uint8_t *data = nullptr;
  size_t len = 0;
  uint8_t op() const { return len ? data[0] : 0; }
  /** Addressed opcodes (0x80+) carry the destination in byte 1: true when it is `node` or everyone. */
  bool for_me(uint8_t node) const { return op() < 0x80 || (len >= 2 && (data[1] == 0 || data[1] == node)); }
};

/** Reassembly per CAN ID (the receiving rules): a few buffers, recycled oldest first, and the last completed transfer
 *  ID per sender and channel for the 1 s duplicate rule. */
class ElcChRx {
 public:
  static const int kBuffers = 6;
  static const int kDups = 48;
  /** True when f completed a transfer (in out). */
  bool feed(const ElcCanFrame &f, uint32_t now_ms, ElcChTransfer &out);
  uint32_t dropped() const { return _dropped; }

 private:
  struct Buf {
    uint16_t id = 0;
    bool busy = false;
    uint8_t tid = 0;
    uint8_t toggle = 0;
    uint32_t ms = 0;
    size_t n = 0;
    uint8_t data[ELC_CH_MAX_PAYLOAD + 2];
  };
  struct Dup {
    uint16_t id = 0;
    uint8_t tid = 0;
    uint32_t ms = 0;
    bool used = false;
  };
  bool complete(uint16_t id, uint8_t tid, const uint8_t *p, size_t n, uint32_t now_ms, ElcChTransfer &out);
  Buf _buf[kBuffers];
  Dup _dup[kDups];
  uint8_t _single[8];
  uint32_t _dropped = 0;
};

// ---- payload helpers (little-endian, as on the rest of the bus) ----
class ElcChWriter {
 public:
  ElcChWriter &u8(uint8_t v);
  ElcChWriter &u16(uint16_t v);
  ElcChWriter &u32(uint32_t v);
  ElcChWriter &s8(int8_t v) { return u8((uint8_t)v); }
  ElcChWriter &s32(int32_t v) { return u32((uint32_t)v); }
  ElcChWriter &bytes(const void *p, size_t n);
  /** Text without a terminator, cut at max bytes. */
  ElcChWriter &text(const char *s, size_t max = ELC_CH_MAX_PAYLOAD);
  const uint8_t *data() const { return _b; }
  size_t size() const { return _n; }
  bool ok() const { return !_over; }

 private:
  uint8_t _b[ELC_CH_MAX_PAYLOAD];
  size_t _n = 0;
  bool _over = false;
};

class ElcChReader {
 public:
  ElcChReader(const uint8_t *p, size_t n) : _p(p), _n(n) {}
  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  int8_t s8() { return (int8_t)u8(); }
  int32_t s32() { return (int32_t)u32(); }
  /** The rest as text into out (NUL-terminated, cut to cap - 1). */
  size_t rest_text(char *out, size_t cap);
  /** Text up to the next NUL (consumed) or the end. */
  size_t field(char *out, size_t cap);
  size_t left() const { return _n - _i; }
  bool ok() const { return !_short; }

 private:
  const uint8_t *_p;
  size_t _n;
  size_t _i = 0;
  bool _short = false;
};

// ---- NET (0x3C0 + node) ----
static const uint8_t ELC_NET_ANNOUNCE = 0x01;
static const uint8_t ELC_NET_GATEWAY_STATUS = 0x11;
static const uint8_t ELC_NET_QUERY = 0x81;
static const uint8_t ELC_NET_IDENTIFY = 0x82;
static const uint8_t ELC_NET_RESTART = 0x83;
static const uint8_t ELC_NET_RAIL = 0x84;
static const uint8_t ELC_NET_GATEWAY_CMD = 0x8B;

static const uint8_t ELC_NET_Q_ANNOUNCE = 0x01;
static const uint8_t ELC_NET_Q_SENSORS = 0x02;
static const uint8_t ELC_NET_Q_ALERTS = 0x04;
static const uint8_t ELC_NET_Q_UI = 0x08;
static const uint8_t ELC_NET_Q_ALL = 0x0F;
static const uint8_t ELC_NET_RESTART_MAGIC = 0x5A;

// Node types (heartbeat and ANNOUNCE) and products (can_channels.md, Registries).
static const uint8_t ELC_TYPE_DISPLAY = 0, ELC_TYPE_AUDIO_BRIDGE = 1, ELC_TYPE_CONSOLE = 2, ELC_TYPE_SENSOR = 3,
                     ELC_TYPE_INPUT = 4, ELC_TYPE_AUDIO_SINK = 5, ELC_TYPE_OUTPUT = 6, ELC_TYPE_GATEWAY = 7,
                     ELC_TYPE_VOICE = 8, ELC_TYPE_RADIO = 9, ELC_TYPE_SERVICE = 0x7F;
static const uint16_t ELC_PRODUCT_DISPLAY = 0x0001, ELC_PRODUCT_AUDIO_BRIDGE = 0x0002, ELC_PRODUCT_CONSOLE = 0x0003,
                      ELC_PRODUCT_AUDIO_CARD = 0x0004, ELC_PRODUCT_GPS = 0x0005, ELC_PRODUCT_LTE = 0x0006,
                      ELC_PRODUCT_VOICE = 0x0007, ELC_PRODUCT_SENSOR_CARD = 0x0008, ELC_PRODUCT_RADIO = 0x0009;

struct ElcNetAnnounce {
  uint8_t version = 1;
  uint8_t type = 0;
  uint16_t product = 0;
  uint16_t fw = 0;
  uint32_t uid = 0;
  uint8_t generation = 1;
  uint32_t produces = 0;
  uint32_t consumes = 0;
  uint8_t flags = 0;  // bit0 factory defaults, bits 1-3 slot (0 = none)
  char name[25] = {0};
  uint8_t slot() const { return (flags >> 1) & 7; }
};
size_t elc_net_encode_announce(const ElcNetAnnounce &a, uint8_t *out, size_t cap);
bool elc_net_decode_announce(const uint8_t *p, size_t n, ElcNetAnnounce &out);

struct ElcNetRail {
  uint8_t dest = 0;
  uint8_t slot = 0;
  uint8_t state = 0;  // 0 request, 1 granted, 2 release / denied / withdrawn
  uint16_t rate10 = 4800;
};
size_t elc_net_encode_rail(const ElcNetRail &r, uint8_t *out, size_t cap);
bool elc_net_decode_rail(const uint8_t *p, size_t n, ElcNetRail &out);

// ---- the service a board runs ----
class ElcChannels {
 public:
  using SendFn = bool (*)(const ElcCanFrame &f, void *ctx);
  using TransferFn = void (*)(const ElcChTransfer &t, void *ctx);
  /** Asked for descriptions (QUERY bits 1-3 from someone, or describe()): send SENSOR / ALERT DEFINEs, UI ITEMs. */
  using DescribeFn = void (*)(uint8_t what, void *ctx);
  using EventFn = void (*)(uint8_t op, const uint8_t *p, size_t n, uint8_t src, void *ctx);

  struct Node {
    bool announced = false;
    uint32_t ms = 0;  // last ANNOUNCE
    ElcNetAnnounce a;
  };

  /** me.name / type / product / produces / consumes describe this board. NET is added to both maps. */
  void begin(uint8_t node, const ElcNetAnnounce &me, SendFn send, void *send_ctx);
  void onTransfer(TransferFn fn, void *ctx) {
    _on_transfer = fn;
    _on_transfer_ctx = ctx;
  }
  void onDescribe(DescribeFn fn, void *ctx) {
    _on_describe = fn;
    _on_describe_ctx = ctx;
  }
  /** IDENTIFY (seconds, 0 = stop) and RESTART addressed to this node. */
  void onNetCommand(EventFn fn, void *ctx) {
    _on_net = fn;
    _on_net_ctx = ctx;
  }
  bool active() const { return _send != nullptr; }
  uint8_t node() const { return _node; }

  /** Every received frame (ElcCan does this once attached). */
  void feed(const ElcCanFrame &f, uint32_t now_ms);
  /** Sends the boot ANNOUNCE and descriptions once, and answers queries after (node x 8) ms. Call often. */
  void poll(uint32_t now_ms);

  /** One transfer on base + this node. False if it did not fit or a frame could not be queued. */
  bool send(uint16_t base, const uint8_t *payload, size_t len);
  bool send(uint16_t base, const ElcChWriter &w) { return w.ok() && send(base, w.data(), w.size()); }
  void announce();
  void query(uint8_t dest, uint8_t what);
  /** Bumps the description generation (a description changed) and re-announces. */
  void descriptionsChanged();
  void setFactoryDefaults(bool on);
  void setSlot(uint8_t slot);

  const Node &nodeInfo(uint8_t n) const { return _nodes[n & 0x1F]; }
  /** First announced node of a type (and product, unless 0), or 0. */
  uint8_t findNode(uint8_t type, uint16_t product = 0) const;
  /** First announced node that consumes a channel (the audio sink consumes AUDIO_CMD), or 0. */
  uint8_t findConsumer(uint16_t base) const;
  uint32_t transfersIn() const { return _in; }
  uint32_t transfersOut() const { return _out; }
  uint32_t dropped() const { return _rx.dropped() + _send_fail; }
  /** One-line summary for bench consoles: the announced nodes. */
  void describe(char *out, size_t n) const;

 private:
  uint8_t _node = 0;
  ElcNetAnnounce _me;
  SendFn _send = nullptr;
  void *_send_ctx = nullptr;
  TransferFn _on_transfer = nullptr;
  void *_on_transfer_ctx = nullptr;
  DescribeFn _on_describe = nullptr;
  void *_on_describe_ctx = nullptr;
  EventFn _on_net = nullptr;
  void *_on_net_ctx = nullptr;
  ElcChRx _rx;
  uint8_t _tid[32] = {0};  // per channel number
  Node _nodes[32];
  bool _booted = false;
  uint8_t _pending_what = 0;
  uint32_t _pending_at = 0;
  uint32_t _in = 0, _out = 0, _send_fail = 0;
};
