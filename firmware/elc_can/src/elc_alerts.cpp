#include "elc_alerts.h"

#include <stdio.h>
#include <string.h>

static const uint32_t kReplayGuardMs = 500;  // a DONE this soon after our PLAY is the old voice's, not a cut-short one

void ElcAlertManager::begin(ElcChannels *ch) { _ch = ch; }

uint16_t ElcAlertManager::defaultSound(uint8_t severity) {
  switch (severity) {
    case ELC_SEV_ADVISORY: return 2;
    case ELC_SEV_CAUTION: return 3;
    case ELC_SEV_WARNING: return 4;
    default: return 0;
  }
}

// ---- table

ElcAlertManager::Alert *ElcAlertManager::find(uint8_t node, uint8_t alert) {
  for (int i = 0; i < _n; i++) {
    if (_a[i].node == node && _a[i].alert == alert) return &_a[i];
  }
  return nullptr;
}

ElcAlertManager::Alert *ElcAlertManager::findOrAdd(uint8_t node, uint8_t alert) {
  Alert *a = find(node, alert);
  if (a) return a;
  if (_n >= kMaxAlerts) {
    // Full: reuse a slot that shows nothing (undefined, or neither active nor latched).
    for (int i = 0; i < _n; i++) {
      if (!_a[i].active && !_a[i].latched) {
        a = &_a[i];
        break;
      }
    }
    if (!a) {
      _dropped++;
      return nullptr;
    }
  } else {
    a = &_a[_n++];
  }
  *a = Alert();
  a->node = node;
  a->alert = alert;
  return a;
}

void ElcAlertManager::setActive(Alert &a, bool on, uint32_t now_ms) {
  if (on && !a.active) {
    a.active = true;
    a.since_ms = now_ms;
    a.acked = false;
    a.latched = false;
    a.rang_sev = 0xFF;
  } else if (!on && a.active) {
    a.active = false;
    a.latched = (a.flags & ELC_ALERT_F_LATCH) && !a.acked;
    a.acked = false;  // re-arms: the next activation is new
    a.rang_sev = 0xFF;
  }
}

void ElcAlertManager::applyDefine(uint8_t node, const ElcAlertDefine &d) {
  Alert *a = findOrAdd(node, d.alert);
  if (!a) return;
  a->defined = true;
  a->severity = d.severity > ELC_SEV_WARNING ? ELC_SEV_WARNING : d.severity;
  a->flags = d.flags;
  a->icon = d.icon;
  a->sound = d.sound;
  elc_ch_copy_text(a->text, sizeof(a->text), d.text);
  const NodeState &ns = _node[node & 31];
  if (ns.seen) setActive(*a, (ns.active >> d.alert) & 1u, _now);
}

void ElcAlertManager::applyState(uint8_t node, uint32_t active, uint32_t now_ms) {
  NodeState &ns = _node[node & 31];
  ns.active = active;
  ns.state_ms = now_ms;
  ns.seen = true;
  _lost &= ~(1u << (node & 31));
  for (int i = 0; i < _n; i++) {
    if (_a[i].node == node) setActive(_a[i], (active >> _a[i].alert) & 1u, now_ms);
  }
  for (uint8_t b = 0; b < 32; b++) {
    if (!((active >> b) & 1u)) continue;
    Alert *a = findOrAdd(node, b);
    if (!a) continue;
    setActive(*a, true, now_ms);
    if (!a->defined) maybeQuery(node, now_ms);
  }
}

void ElcAlertManager::maybeQuery(uint8_t node, uint32_t now_ms) {
  if (!_ch || node == _ch->node()) return;
  NodeState &ns = _node[node & 31];
  if (ns.asked && now_ms - ns.asked_ms < kQueryGapMs) return;
  ns.asked = true;
  ns.asked_ms = now_ms;
  _ch->query(node, ELC_NET_Q_ALERTS);
}

// ---- input

void ElcAlertManager::feed(const ElcChTransfer &t, uint32_t now_ms) {
  _now = now_ms;
  if (!_ch) return;
  if (t.base == ELC_CH_ALERT && t.src != _ch->node()) {
    ElcAlertDefine d;
    ElcAlertState s;
    if (elc_alert_decode(t, d)) {
      applyDefine(t.src, d);
    } else if (elc_alert_decode(t, s)) {
      NodeState &ns = _node[t.src & 31];
      const bool changed = ns.seen && s.generation != ns.generation;
      ns.generation = s.generation;
      applyState(t.src, s.active, now_ms);
      if (changed) {
        ns.asked = false;  // a description changed: ask now
        maybeQuery(t.src, now_ms);
      }
    }
  } else if (t.base == ELC_CH_AUDIO_CMD && t.op() == ELC_AUDIO_DONE && t.len >= 4) {
    ElcChReader r(t.data + 1, t.len - 1);
    const uint8_t node = r.u8();
    const uint16_t sound = r.u16();
    if (r.ok() && node == _ch->node() && _ring_sound && sound == _ring_sound && now_ms - _ring_ms >= kReplayGuardMs) {
      _ring_replay = true;  // the sink stopped our warning chime while it should still ring
    }
  }
}

// ---- own alerts

void ElcAlertManager::defineOwn(const ElcAlertDefine &d) {
  if (d.alert >= kMaxOwn || !_ch) return;
  const uint32_t bit = 1u << d.alert;
  ElcAlertDefine &o = _own[d.alert];
  if (_own_defined & bit) {
    if (o.severity == d.severity && o.flags == d.flags && o.icon == d.icon && o.sound == d.sound &&
        strncmp(o.text, d.text, sizeof(o.text)) == 0) {
      return;
    }
    _own_generation = (uint8_t)(_own_generation + 1);
    if (!_own_generation) _own_generation = 1;
    _own_state_dirty = true;
  }
  o = d;
  o.text[sizeof(o.text) - 1] = '\0';
  _own_defined |= bit;
  _own_define_pending |= bit;
  applyDefine(_ch->node(), o);
}

void ElcAlertManager::setOwn(uint8_t alert, bool active) {
  if (alert >= kMaxOwn || !_ch) return;
  const uint32_t bit = 1u << alert;
  const uint32_t next = active ? (_own_active | bit) : (_own_active & ~bit);
  if (next == _own_active) return;
  _own_active = next;
  _own_state_dirty = true;
  applyState(_ch->node(), _own_active, _now);
}

void ElcAlertManager::sendOwnState(uint32_t now_ms) {
  if (!_own_defined) return;  // a node that defines alerts sends STATE, always
  if (!_own_state_dirty && now_ms - _own_state_ms < kStateMs) return;
  ElcAlertState s;
  s.active = _own_active;
  s.generation = _own_generation;
  ElcChWriter w;
  elc_alert_encode(s, w);
  if (_ch->send(ELC_CH_ALERT, w)) {
    _own_state_dirty = false;
    _own_state_ms = now_ms;
  }
}

// ---- the driver

bool ElcAlertManager::ack(uint8_t node, uint8_t alert, uint32_t now_ms) {
  _now = now_ms;
  Alert *a = find(node, alert);
  if (!a || !a->listed() || a->acked) return false;
  if (a->latched) {
    a->latched = false;  // cleared already: acknowledging removes it
  } else {
    a->acked = true;
  }
  if (_ch) {
    ElcAlertAck k;
    k.dest = node;
    k.alert = alert;
    ElcChWriter w;
    elc_alert_encode(k, w);
    _ch->send(ELC_CH_ALERT, w);
  }
  sounds(now_ms);  // a warning chime stops at once
  sendSummary(now_ms, false);
  return true;
}

bool ElcAlertManager::ackTop(uint32_t now_ms) {
  const Alert *t = top();
  return t && ack(t->node, t->alert, now_ms);
}

int ElcAlertManager::ackAll(uint32_t now_ms) {
  int k = 0;
  while (ackTop(now_ms)) k++;
  return k;
}

// ---- views

bool ElcAlertManager::urgentBefore(const Alert &a, const Alert &b) {
  const bool ua = !a.acked, ub = !b.acked;
  if (ua != ub) return ua;
  if (a.active != b.active) return a.active;  // a latched (cleared) one after the live ones
  if (a.severity != b.severity) return a.severity > b.severity;
  return (int32_t)(a.since_ms - b.since_ms) > 0;  // newest first
}

int ElcAlertManager::list(const Alert **out, int max) const {
  int k = 0;
  for (int i = 0; i < _n && k < max; i++) {
    if (!_a[i].listed()) continue;
    int j = k++;
    while (j > 0 && urgentBefore(_a[i], *out[j - 1])) {
      out[j] = out[j - 1];
      j--;
    }
    out[j] = &_a[i];
  }
  return k;
}

const ElcAlertManager::Alert *ElcAlertManager::top() const {
  const Alert *best = nullptr;
  for (int i = 0; i < _n; i++) {
    const Alert &a = _a[i];
    if (!a.listed() || a.acked) continue;
    if (!best || urgentBefore(a, *best)) best = &a;
  }
  return best;
}

ElcAlertSummary ElcAlertManager::summary() const {
  ElcAlertSummary s;
  for (int i = 0; i < _n; i++) {
    const Alert &a = _a[i];
    if (!a.defined || !a.active) continue;
    s.count++;
    if (s.top_sev == 0xFF || a.severity > s.top_sev) s.top_sev = a.severity;
    if (!a.acked && (s.top_unacked == 0xFF || a.severity > s.top_unacked)) s.top_unacked = a.severity;
  }
  const Alert *t = top();
  if (t) {
    s.top_node = t->node;
    s.top_alert = t->alert;
  }
  return s;
}

void ElcAlertManager::sendSummary(uint32_t now_ms, bool force) {
  if (!_ch) return;
  const ElcAlertSummary s = summary();
  const bool same = _sum_valid && s.top_sev == _sum_sent.top_sev && s.top_unacked == _sum_sent.top_unacked &&
                    s.count == _sum_sent.count && s.top_node == _sum_sent.top_node &&
                    s.top_alert == _sum_sent.top_alert;
  if (!force && same && now_ms - _sum_ms < kStateMs) return;
  ElcChWriter w;
  elc_alert_encode(s, w);
  if (_ch->send(ELC_CH_ALERT, w)) {
    _sum_sent = s;
    _sum_valid = true;
    _sum_ms = now_ms;
  }
}

void ElcAlertManager::describe(char *out, size_t n) const {
  if (!n) return;
  static const char *const kSev[4] = {"info", "advisory", "caution", "warning"};
  const Alert *l[kMaxAlerts];
  const int k = list(l, kMaxAlerts);
  size_t used = (size_t)snprintf(out, n, "alerts: %d listed, ringing %u, lost nodes %08lX, dropped %lu", k,
                                 (unsigned)_ring_sound, (unsigned long)_lost, (unsigned long)_dropped);
  for (int i = 0; i < k && used < n; i++) {
    const Alert &a = *l[i];
    used += (size_t)snprintf(out + used, n - used, "\n  0x%02X #%u %-8s %s%s%s %s, %lus", (unsigned)a.node,
                             (unsigned)a.alert, kSev[a.severity & 3], a.acked ? "acked" : "NEW",
                             a.latched ? " latched" : "", nodeLost(a.node) ? " lost" : "", a.text,
                             (unsigned long)((_now - a.since_ms) / 1000u));
  }
}

// ---- chimes

uint8_t ElcAlertManager::sink() const {
  const uint8_t s = _ch ? _ch->findConsumer(ELC_CH_AUDIO_CMD) : 0;
  return s;  // 0 = everyone: the sink is the only node that acts on PLAY / STOP
}

uint16_t ElcAlertManager::soundOf(const Alert &a) const {
  if (a.severity == ELC_SEV_INFO || (a.flags & ELC_ALERT_F_SILENT) || a.sound == 0xFFFF) return 0;
  return a.sound ? a.sound : defaultSound(a.severity);
}

void ElcAlertManager::play(uint16_t sound, uint8_t priority, uint8_t repeat) {
  ElcAudioPlay p;
  p.dest = sink();
  p.sound = sound;
  p.zones_prio = (uint8_t)(priority & 0x0F);  // zones 0: the sink's default
  p.level = 100;
  p.repeat = repeat;
  ElcChWriter w;
  elc_audio_encode(p, w);
  _ch->send(ELC_CH_AUDIO_CMD, w);
}

void ElcAlertManager::stop(uint16_t sound) {
  ElcAudioStop s;
  s.dest = sink();
  s.sound = sound;
  ElcChWriter w;
  elc_audio_encode(s, w);
  _ch->send(ELC_CH_AUDIO_CMD, w);
}

void ElcAlertManager::sounds(uint32_t now_ms) {
  if (!_ch) return;
  const Alert *ring = nullptr;
  for (int i = 0; i < _n; i++) {
    Alert &a = _a[i];
    if (!a.defined || !a.active || a.acked) continue;
    const uint16_t snd = soundOf(a);
    if (a.severity == ELC_SEV_WARNING) {
      a.rang_sev = a.severity;
      if (snd && (!ring || urgentBefore(a, *ring))) ring = &a;
    } else if (a.rang_sev == 0xFF || a.severity > a.rang_sev) {  // new, or its severity rose
      a.rang_sev = a.severity;
      if (snd) play(snd, (uint8_t)(a.severity + 1), 1);  // advisory 2, caution 3
    }
  }
  const uint16_t want = ring ? soundOf(*ring) : 0;
  if (want != _ring_sound) {
    if (_ring_sound) stop(_ring_sound);
    if (want) play(want, 4, 0);
    _ring_sound = want;
    _ring_ms = now_ms;
    _ring_replay = false;
  } else if (_ring_replay && want) {
    play(want, 4, 0);
    _ring_ms = now_ms;
    _ring_replay = false;
  }
}

// ---- poll

void ElcAlertManager::poll(uint32_t now_ms) {
  _now = now_ms;
  if (!_ch || !_ch->active()) return;
  const uint8_t self = _ch->node();
  // Source lost: a node with alerts whose STATE stopped. Its alerts stay listed, greyed by the board.
  for (int i = 0; i < _n; i++) {
    const uint8_t node = _a[i].node;
    if (node == self) continue;
    const NodeState &ns = _node[node & 31];
    if (ns.seen && now_ms - ns.state_ms > kLostMs) _lost |= 1u << (node & 31);
    if (_a[i].active && !_a[i].defined) maybeQuery(node, now_ms);
  }
  // Own DEFINEs, one at a time.
  if (_own_define_pending && (!_own_define_sent || now_ms - _own_define_ms >= kDefineGapMs)) {
    uint8_t b = 0;
    while (!((_own_define_pending >> b) & 1u)) b++;
    ElcChWriter w;
    elc_alert_encode(_own[b], w);
    if (_ch->send(ELC_CH_ALERT, w)) _own_define_pending &= ~(1u << b);
    _own_define_ms = now_ms;
    _own_define_sent = true;
  }
  sendOwnState(now_ms);
  sounds(now_ms);
  sendSummary(now_ms, false);
}
