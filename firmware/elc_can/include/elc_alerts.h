#pragma once

// The alert manager (docs/protocols/can_channels.md, ALERT): the one alert system of the dash. It runs on the center
// and collects every node's alerts (DEFINE + STATE), publishes the center's own on the same channel as node 0x01,
// plays the chimes on the audio sink (AUDIO_CMD PLAY / STOP), tracks acknowledgement (ACK back to the source),
// greys a node's alerts when its STATE stops ("source lost") and sends SUMMARY for simple nodes.
//
// It only keeps the list: the board decides how alerts show (the menu screen's banner and list) and calls ack() when
// the driver acknowledges. No Arduino or ESP-IDF dependencies: host-tested (test/host/test_elc_alerts.cpp).
//
// Chimes, per alert (never for info, a silent flag or sound 0xFFFF):
//   - advisory / caution: one play when the alert becomes active (or its severity rises), at priority 2 / 3;
//   - warning: repeats (PLAY repeat 0) while any warning is active and unacknowledged, at priority 4, then STOP.
//     When the sink reports DONE for it while it should still ring (the sink cut it short), it is played again.
// Sound 0 in a DEFINE = the severity's default: 2 soft chime, 3 caution chime, 4 warning chime.

#include <stddef.h>
#include <stdint.h>

#include "elc_chan.h"
#include "elc_chan_msg.h"

class ElcAlertManager {
 public:
  static const int kMaxAlerts = 48;     // every node's alerts together (each node may define up to 32)
  static const int kMaxOwn = 32;
  static const uint32_t kLostMs = 3000;      // STATE silent this long: the node's alerts are greyed
  static const uint32_t kStateMs = 1000;     // own STATE and SUMMARY, and at once on a change
  static const uint32_t kQueryGapMs = 2000;  // per node, asking for missing / changed descriptions
  static const uint32_t kDefineGapMs = 25;   // own DEFINEs, one at a time (a DEFINE is up to 8 frames)

  struct Alert {
    uint8_t node = 0, alert = 0;
    bool defined = false;   // a DEFINE arrived (until then only the STATE bit is known)
    uint8_t severity = 0, flags = 0, icon = 0;
    uint16_t sound = 0;
    char text[49] = {0};
    bool active = false;
    bool acked = false;     // acknowledged during this activation (cleared when the alert clears)
    bool latched = false;   // cleared but listed until acknowledged (flag bit1)
    uint32_t since_ms = 0;  // when it became active
    uint8_t rang_sev = 0xFF;  // the severity its one-shot chime was played for (0xFF = not yet)
    /** Shown in the list: active, or latched. */
    bool listed() const { return defined && (active || latched); }
  };

  /** ch: the board's channel node (sends on base + ch.node(), finds the sink, names nodes). */
  void begin(ElcChannels *ch);

  /** Every received transfer (ALERT from other nodes, AUDIO_CMD DONE from the sink). Others are ignored. */
  void feed(const ElcChTransfer &t, uint32_t now_ms);
  /** Call often: own STATE / DEFINEs, SUMMARY, source lost, chimes, queries. */
  void poll(uint32_t now_ms);
  /** QUERY bit2 for this node (or boot): resend the own DEFINEs. */
  void describeOwn() { _own_define_pending = _own_defined; }

  // ---- the board's own alerts (sent as this node; the board does not hear its own transfers)
  /** Define or update one (text, severity...). A change is re-sent and bumps the description generation. */
  void defineOwn(const ElcAlertDefine &d);
  void setOwn(uint8_t alert, bool active);
  bool ownActive(uint8_t alert) const { return alert < 32 && (_own_active >> alert) & 1u; }

  // ---- the driver
  /** Acknowledge one alert: ACK to its source, its warning chime stops. False when it is not listed. */
  bool ack(uint8_t node, uint8_t alert, uint32_t now_ms);
  /** Acknowledge the top unacknowledged alert (the banner's Select). False when there is none. */
  bool ackTop(uint32_t now_ms);
  /** Acknowledge everything listed. Returns how many. */
  int ackAll(uint32_t now_ms);

  // ---- views
  /** Listed alerts, most urgent first: unacknowledged before acknowledged, then severity, then newest. */
  int list(const Alert **out, int max) const;
  /** The alert the banner shows: the most urgent unacknowledged one, else nullptr. */
  const Alert *top() const;
  ElcAlertSummary summary() const;
  /** Nodes whose STATE stopped (bit n = node n), among those that defined alerts. */
  uint32_t lostNodes() const { return _lost; }
  bool nodeLost(uint8_t node) const { return (_lost >> (node & 31)) & 1u; }
  /** The sound ringing now (repeating warning), 0 = none. */
  uint16_t ringing() const { return _ring_sound; }
  uint32_t dropped() const { return _dropped; }
  /** One line per listed alert, for bench consoles. */
  void describe(char *out, size_t n) const;

  /** Severity's default sound (0 for info). */
  static uint16_t defaultSound(uint8_t severity);

 private:
  struct NodeState {
    bool seen = false;       // a STATE arrived
    uint32_t active = 0;
    uint8_t generation = 0;
    uint32_t state_ms = 0;
    uint32_t asked_ms = 0;
    bool asked = false;
  };

  Alert *find(uint8_t node, uint8_t alert);
  Alert *findOrAdd(uint8_t node, uint8_t alert);
  void applyDefine(uint8_t node, const ElcAlertDefine &d);
  void applyState(uint8_t node, uint32_t active, uint32_t now_ms);
  void setActive(Alert &a, bool on, uint32_t now_ms);
  void maybeQuery(uint8_t node, uint32_t now_ms);
  void sounds(uint32_t now_ms);
  uint16_t soundOf(const Alert &a) const;
  uint8_t sink() const;
  void play(uint16_t sound, uint8_t priority, uint8_t repeat);
  void stop(uint16_t sound);
  void sendOwnState(uint32_t now_ms);
  void sendSummary(uint32_t now_ms, bool force);
  static bool urgentBefore(const Alert &a, const Alert &b);

  ElcChannels *_ch = nullptr;
  Alert _a[kMaxAlerts];
  int _n = 0;
  NodeState _node[32];
  uint32_t _lost = 0;
  uint32_t _dropped = 0;

  ElcAlertDefine _own[kMaxOwn];
  uint32_t _own_defined = 0, _own_active = 0, _own_define_pending = 0;
  uint8_t _own_generation = 1;
  bool _own_state_dirty = true;
  bool _own_define_sent = false;
  uint32_t _own_state_ms = 0, _own_define_ms = 0;

  ElcAlertSummary _sum_sent;
  bool _sum_valid = false;
  uint32_t _sum_ms = 0;

  uint16_t _ring_sound = 0;
  uint32_t _ring_ms = 0;
  bool _ring_replay = false;
  uint32_t _now = 0;
};
