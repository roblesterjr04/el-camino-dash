#pragma once

// BULK (docs/protocols/can_channels.md, BULK): moving an object larger than a transfer. Control messages are
// transfers on 0x320 + node; data goes raw on 0x340 + the sender of the data, 5 object bytes per frame behind a
// 3-byte session / chunk number. The receiving side drives the flow with WANT (first chunk it misses, credit), so a
// lost frame costs one re-ask and nothing is sent that nobody asked for.
//
// This file has the message codecs and both ends of a WRITE (the side pushing an object to other nodes):
//   ElcBulkWriter   pushes one object to one node or, for dest 0, to every node that accepts it (multicast: the
//                   data frames are on the writer's ID, so one frame feeds every receiver that is at that chunk).
//   ElcBulkReceiver takes one object from a writer and hands it, in order, to a sink.
// READ (an owner sending an object it holds) is in the spec; no node implements it yet.
//
// No Arduino or ESP-IDF dependencies: host-tested (test/host/test_elc_bulk.cpp).

#include <stddef.h>
#include <stdint.h>

#include "elc_chan.h"

// ---- opcodes, kinds, results ----
static const uint8_t ELC_BULK_INFO = 0x01;
static const uint8_t ELC_BULK_READ = 0x81;
static const uint8_t ELC_BULK_WRITE = 0x82;
static const uint8_t ELC_BULK_WANT = 0x83;
static const uint8_t ELC_BULK_END = 0x84;

static const uint8_t ELC_BULK_KIND_SOUND = 1;
static const uint8_t ELC_BULK_KIND_FIRMWARE = 2;
static const uint8_t ELC_BULK_KIND_SD_FILE = 3;

static const uint8_t ELC_BULK_REFUSED = 0xFF;  // INFO session: the WRITE / READ is refused

// END results
static const uint8_t ELC_BULK_OK = 0;
static const uint8_t ELC_BULK_CRC = 1;
static const uint8_t ELC_BULK_NO_SPACE = 2;
static const uint8_t ELC_BULK_ABORTED = 3;
static const uint8_t ELC_BULK_REJECTED = 4;  // the content is not for this node, or failed its own checks
static const uint8_t ELC_BULK_PENDING = 0xFE;  // not on the wire: a sink still finishing (below)

// INFO refusal reasons (the byte after CRC-32 when the session is ELC_BULK_REFUSED)
static const uint8_t ELC_BULK_WHY_NONE = 0;
static const uint8_t ELC_BULK_WHY_KIND = 1;     // this node takes no such kind / object
static const uint8_t ELC_BULK_WHY_TARGET = 2;   // firmware for another product or build
static const uint8_t ELC_BULK_WHY_SIZE = 3;     // too big for this node
static const uint8_t ELC_BULK_WHY_BUSY = 4;     // already in a transfer
static const uint8_t ELC_BULK_WHY_NOT_NOW = 5;  // the vehicle is moving, or the node will not take it now

static const uint32_t ELC_BULK_CHUNK_BYTES = 5;
static const uint32_t ELC_BULK_MAX_SIZE = (1u << 21) * ELC_BULK_CHUNK_BYTES;  // 21-bit chunk number: 10 MB

/** CRC-32 (IEEE 802.3, as in zlib): init 0xFFFFFFFF, reflected, final XOR. Pass the previous value to continue. */
uint32_t elc_crc32(const uint8_t *p, size_t n, uint32_t crc = 0);

// ---- messages ----
struct ElcBulkInfo {
  uint8_t to = 0;  // the node that asked (the writer of a WRITE)
  uint8_t tag = 0;
  uint8_t session = 0;  // ELC_BULK_REFUSED = refused
  uint32_t size = 0;
  uint32_t crc = 0;
  uint8_t why = ELC_BULK_WHY_NONE;  // refused only
};
struct ElcBulkWrite {
  uint8_t dest = 0;  // 0 = every node that takes it (firmware: every node of that product and build)
  uint8_t tag = 0;
  uint8_t kind = 0;
  uint16_t object = 0;  // firmware: the product number
  uint32_t size = 0;
  uint32_t crc = 0;     // firmware: the image's CRC-32 from the file's header (the header has its own)
  uint8_t session = 0;  // the writer's data session (0..7)
  // firmware only (kind 2)
  uint16_t fw = 0;
  char variant[33] = {0};  // the build (PlatformIO environment), e.g. "display-waveshare4c"
};
struct ElcBulkWant {
  uint8_t dest = 0;  // the sender of the data
  uint8_t session = 0;
  uint32_t first = 0;  // chunk
  uint8_t credit = 0;  // chunks it can take now, from first
};
struct ElcBulkEnd {
  uint8_t dest = 0;  // the other end (or 0 from a writer aborting everyone)
  uint8_t session = 0;
  uint8_t result = ELC_BULK_OK;
};

size_t elc_bulk_encode(const ElcBulkInfo &m, uint8_t *out, size_t cap);
size_t elc_bulk_encode(const ElcBulkWrite &m, uint8_t *out, size_t cap);
size_t elc_bulk_encode(const ElcBulkWant &m, uint8_t *out, size_t cap);
size_t elc_bulk_encode(const ElcBulkEnd &m, uint8_t *out, size_t cap);
bool elc_bulk_decode(const uint8_t *p, size_t n, ElcBulkInfo &out);
bool elc_bulk_decode(const uint8_t *p, size_t n, ElcBulkWrite &out);
bool elc_bulk_decode(const uint8_t *p, size_t n, ElcBulkWant &out);
bool elc_bulk_decode(const uint8_t *p, size_t n, ElcBulkEnd &out);

/** A data frame on 0x340 + node: session (top 3 bits) and chunk (21 bits), then up to 5 bytes. */
void elc_bulk_data_frame(uint8_t node, uint8_t session, uint32_t chunk, const uint8_t *p, size_t n, ElcCanFrame &out);
bool elc_bulk_data_decode(const ElcCanFrame &f, uint8_t *session, uint32_t *chunk, const uint8_t **p, size_t *n);
static inline uint32_t elc_bulk_chunks(uint32_t size) { return (size + ELC_BULK_CHUNK_BYTES - 1) / ELC_BULK_CHUNK_BYTES; }

// ---- the writing side ----
class ElcBulkWriter : public ElcChService {
 public:
  /** Raw data frames go out through this (with hardware retry where the board can: see ElcCan::send). False = the
   *  transmit queue is full; the chunk is offered again on the next poll. */
  using RawSendFn = bool (*)(const ElcCanFrame &f, void *ctx);
  /** Up to n bytes of the object from offset into buf. Returns how many are available now: 0 when a streaming source
   *  has not received them yet (the writer waits), fewer than n only at the end of the object. */
  using ReadFn = size_t (*)(uint32_t offset, uint8_t *buf, size_t n, void *ctx);

  static const int kMaxReceivers = 8;
  static const uint32_t kCollectMs = 600;   // a WRITE to everyone waits this long for INFOs before the data starts
  static const uint32_t kWriteRetryMs = 1000;
  static const int kWriteTries = 3;
  static const uint32_t kSilentMs = 6000;   // a receiver not heard from for this long is given up

  enum class State : uint8_t { Idle, Offering, Sending, Done };
  enum class RxState : uint8_t { Accepted, Done, Failed, Refused };
  struct Receiver {
    uint8_t node = 0;
    RxState state = RxState::Accepted;
    uint8_t result = 0;   // END result, or the INFO refusal reason
    uint32_t acked = 0;   // first chunk of its last WANT: it holds everything before
    uint32_t cursor = 0;  // next chunk to send it
    uint32_t end = 0;     // its credit runs to here (exclusive)
    uint32_t heard_ms = 0;
    uint32_t last_first = 0xFFFFFFFFu;  // first chunk of its WANT before
  };

  /** Adds itself to the channels as a service (it hears BULK and is polled with them). */
  void begin(ElcChannels *ch, RawSendFn raw, void *raw_ctx);
  void chTransfer(const ElcChTransfer &t, uint32_t now_ms) override { transfer(t, now_ms); }
  void chPoll(uint32_t now_ms) override { poll(now_ms); }
  /** Offers the object (WRITE) and then serves WANTs until every receiver has ENDed or gone quiet. False if busy. */
  bool start(const ElcBulkWrite &w, ReadFn read, void *read_ctx, uint32_t now_ms);
  /** Every BULK transfer the board hears (ElcChannels passes them when the writer is attached as a service). */
  void transfer(const ElcChTransfer &t, uint32_t now_ms);
  /** Sends data within the pace. Call often. */
  void poll(uint32_t now_ms);
  /** Ends the session for everyone (END aborted to each receiver). */
  void abort(uint32_t now_ms);

  /** Frames per millisecond the writer may send (x10): 25 = one per 0.4 ms, the firmware profile's pace. */
  void setPace(uint16_t frames_per_ms_x10) { _pace_x10 = frames_per_ms_x10 ? frames_per_ms_x10 : 1; }

  State state() const { return _state; }
  bool busy() const { return _state == State::Offering || _state == State::Sending; }
  const ElcBulkWrite &offer() const { return _w; }
  int receivers() const { return _n; }
  const Receiver &receiver(int i) const { return _rx[i]; }
  /** Lowest chunk some receiver still needs (a streaming source may drop data before byte low_water() * 5). */
  uint32_t lowWater() const;
  /** 0..1000: how far the slowest receiver still in the transfer has got. */
  uint16_t progress() const;
  uint32_t framesSent() const { return _frames; }
  uint32_t resends() const { return _resends; }
  /** How the session ended: true when at least one receiver took the whole object and none failed. */
  bool succeeded() const;

 private:
  Receiver *find(uint8_t node);
  void send_write(uint32_t now_ms);
  void finish();

  ElcChannels *_ch = nullptr;
  RawSendFn _raw = nullptr;
  void *_raw_ctx = nullptr;
  ReadFn _read = nullptr;
  void *_read_ctx = nullptr;
  ElcBulkWrite _w;
  State _state = State::Idle;
  Receiver _rx[kMaxReceivers];
  int _n = 0;
  uint32_t _chunks = 0;
  uint32_t _offer_ms = 0, _write_ms = 0, _last_tick = 0;
  int _write_tries = 0;
  uint16_t _pace_x10 = 25;
  uint32_t _tokens_x10 = 0;
  uint32_t _frames = 0, _resends = 0, _high = 0;
  uint8_t _tag = 0x40, _session = 0;
};

// ---- the receiving side ----
class ElcBulkReceiver {
 public:
  /** Where the bytes go, in order. Every call is in object order with no gaps. */
  struct Sink {
    virtual ~Sink() {}
    /** A WRITE for this node: ELC_BULK_WHY_NONE to accept it, or a refusal reason. */
    virtual uint8_t accept(const ElcBulkWrite &w, uint8_t from) = 0;
      /** Bytes at offset. Return an END result other than ELC_BULK_OK to stop the transfer (e.g. a header that is not
     *  for this node: ELC_BULK_REJECTED). */
    virtual uint8_t write(uint32_t offset, const uint8_t *p, size_t n) = 0;
    /** The WRITE's CRC-32 covers the object from this offset (firmware: after its 64-byte header). */
    virtual uint32_t crcFrom() const { return 0; }
    /** Every byte arrived and the CRC matched (crc_ok) or not. Return the END result to send, or ELC_BULK_PENDING
     *  while the sink works on (writing flash): the receiver then asks finishing() each poll and keeps the writer
     *  waiting with an empty WANT once a second. */
    virtual uint8_t finish(bool crc_ok) = 0;
    virtual uint8_t finishing() { return ELC_BULK_OK; }
    /** The transfer stopped without finishing (the writer aborted, went quiet, or write() refused). */
    virtual void aborted(uint8_t result) = 0;
  };

  static const uint8_t kCredit = 96;       // chunks per WANT (480 bytes): within the TWAI receive queue
  static const uint32_t kQuietMs = 300;    // no progress for this long: ask again from the first missing chunk
  static const uint32_t kGapAskMs = 20;    // a chunk past the first missing one (a lost frame): ask for it, at most this often
  static const uint32_t kGiveUpMs = 8000;  // nothing from the writer for this long: abort
  static const uint32_t kKeepAliveMs = 1000;  // while the sink finishes: an empty WANT this often

  enum class State : uint8_t { Idle, Receiving, Finishing, Finished };

  void begin(ElcChannels *ch, Sink *sink);
  /** BULK transfers (WRITE for this node or everyone, END from the writer). */
  void transfer(const ElcChTransfer &t, uint32_t now_ms);
  /** Raw data frames (0x340 + the writer). */
  void frame(const ElcCanFrame &f, uint32_t now_ms);
  void poll(uint32_t now_ms);

  State state() const { return _state; }
  bool busy() const { return _state == State::Receiving || _state == State::Finishing; }
  uint8_t writer() const { return _from; }
  const ElcBulkWrite &offer() const { return _w; }
  uint32_t received() const { return _bytes; }
  uint16_t progress() const { return _w.size ? (uint16_t)((uint64_t)_bytes * 1000 / _w.size) : 0; }
  uint8_t result() const { return _result; }
  uint32_t asks() const { return _asks; }

 private:
  void want(uint32_t now_ms, uint8_t credit = kCredit);
  void end(uint8_t result, uint32_t now_ms);
  void stop(uint8_t result);
  /** The next chunk in order: to the sink and the CRC. False when the sink stopped the transfer. */
  bool take(const uint8_t *p, size_t n, uint32_t now_ms);

  ElcChannels *_ch = nullptr;
  Sink *_sink = nullptr;
  State _state = State::Idle;
  ElcBulkWrite _w;
  uint8_t _from = 0;
  uint32_t _chunks = 0, _next = 0, _granted = 0, _bytes = 0, _crc = 0;
  uint32_t _heard_ms = 0, _ask_ms = 0, _progress_ms = 0, _crc_from = 0;
  uint8_t _result = 0;
  uint8_t _end_repeats = 0;
  uint32_t _end_ms = 0;
  uint32_t _asks = 0;
  // chunks that arrived past a lost one, kept (by chunk % kCredit) until the lost one is repaired: the repair asks for
  // that one chunk alone (WANT credit 1), so a lost frame costs one frame again, not the window after it
  uint32_t _held_chunk[kCredit];
  uint8_t _held[kCredit][ELC_BULK_CHUNK_BYTES];
  bool _held_ok[kCredit] = {false};
};
