#pragma once

// The store's side of the STORE channel (docs/protocols/can_channels.md, STORE): any node PUTs, GETs and DELETEs
// named records, and the node running this keeps them. Each sender only reaches its own records, which are kept
// under an owner name made from its ANNOUNCE ("<product>_<node>", hex: "0005_14"), so a replacement board of the
// same product at the same node number finds them again. The storage itself is a backend (the center's SD card;
// a map in the host tests). No Arduino dependencies: host-tested.

#include <stddef.h>
#include <stdint.h>

#include "elc_chan.h"
#include "elc_chan_msg.h"

/** Where records live. `owner` is "<product>_<node>" (hex), `key` passed elc_store_key_ok(). */
class ElcStoreBackend {
 public:
  virtual ~ElcStoreBackend() {}
  /** False while there is nowhere to store (no SD card): every request is answered NO_MEDIA. */
  virtual bool ready() = 0;
  /** A record's size in bytes, or -1 when there is none. */
  virtual int32_t size(const char *owner, const char *key) = 0;
  /** How many records an owner has. */
  virtual int count(const char *owner) = 0;
  /** Replaces the record (append false) or adds to its end (append true), creating it either way. */
  virtual bool write(const char *owner, const char *key, const uint8_t *p, size_t n, bool append) = 0;
  /** Up to cap bytes from offset into out: the number read, or -1 on an error. */
  virtual int32_t read(const char *owner, const char *key, uint32_t offset, uint8_t *out, size_t cap) = 0;
  virtual bool remove(const char *owner, const char *key) = 0;
};

class ElcStoreServer {
 public:
  /** `ch` sends the answers (from this node); `backend` keeps the records. */
  void begin(ElcChannels *ch, ElcStoreBackend *backend);
  /** Every received transfer: answers the STORE requests addressed to this node. True when it was one. */
  bool handle(const ElcChTransfer &t);

  struct Stats {
    uint32_t puts = 0, gets = 0, deletes = 0, refused = 0;  // refused: answered with a result other than OK
    uint32_t bytes_in = 0, bytes_out = 0;
    uint8_t last_node = 0, last_op = 0, last_result = 0;
  };
  const Stats &stats() const { return _stats; }

  /** "0005_14" for a node that has announced; false (and out empty) before its ANNOUNCE. */
  bool owner(uint8_t node, char *out, size_t cap) const;

 private:
  static const size_t kOwnerCap = 16;
  /** Key, owner and media checks shared by every request; fills own on OK. */
  uint8_t check(uint8_t src, const char *key, char *own);
  uint8_t put(const ElcChTransfer &t, uint8_t *tag, uint32_t *size);
  uint8_t del(const ElcChTransfer &t, uint8_t *tag);
  uint8_t read(uint8_t src, const ElcStoreGet &v, ElcStoreData *d, uint8_t *buf);
  void get(const ElcChTransfer &t);
  void reply(uint8_t to, uint8_t tag, uint8_t result, uint32_t size);
  ElcChannels *_ch = nullptr;
  ElcStoreBackend *_b = nullptr;
  Stats _stats;
};
