#include "accessory_can.h"

#include <new>
#include <stdio.h>
#include <string.h>

namespace {

struct AccNode {
  uint8_t node = 0;  // 0 = free
  uint8_t generation = 0;
  AccItem *items = nullptr;  // grows 16, 32, 64 as items arrive (the console has little RAM)
  int n = 0, cap = 0;
  uint32_t sig = 0;
};

ElcChannels *s_ch = nullptr;
AccNode s_nodes[kAccMaxNodes];
uint32_t s_query_ms[32] = {0};
uint8_t s_view_node = 0, s_view_group = ELC_UI_TOP;
uint32_t s_view_ms = 0;
uint32_t s_rx_items = 0, s_rx_values = 0, s_tx_sets = 0;
uint32_t s_now = 0;  // the time of the last poll or transfer

const uint32_t kViewMs = 1000;      // VIEW keepalive (the accessory streams live items while it is under 3 s old)
const uint32_t kQueryMs = 5000;     // ask a node for its items at most this often
const uint32_t kPendingMs = 1500;   // a SET with no RESULT after this is shown as not answered

uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

AccNode *find(uint8_t node) {
  for (AccNode &n : s_nodes) {
    if (n.node == node) return &n;
  }
  return nullptr;
}

AccNode *find_or_add(uint8_t node) {
  AccNode *n = find(node);
  if (n) return n;
  for (AccNode &x : s_nodes) {
    if (!x.node) {
      x.node = node;
      x.n = 0;
      x.sig = 1;
      return &x;
    }
  }
  return nullptr;  // more accessories than kAccMaxNodes: the extra ones are not shown
}

AccItem *item_of(AccNode *n, uint8_t item, bool add) {
  for (int i = 0; i < n->n; i++) {
    if (n->items[i].d.item == item) return &n->items[i];
  }
  if (!add || n->n >= kAccMaxItems) return nullptr;
  if (n->n == n->cap) {  // grow: pointers handed out earlier are only used within one call
    const int cap = n->cap ? n->cap * 2 : 16;
    AccItem *more = new (std::nothrow) AccItem[cap > kAccMaxItems ? kAccMaxItems : cap];
    if (!more) return nullptr;
    for (int i = 0; i < n->n; i++) more[i] = n->items[i];
    delete[] n->items;
    n->items = more;
    n->cap = cap > kAccMaxItems ? kAccMaxItems : cap;
  }
  AccItem *it = &n->items[n->n++];
  *it = AccItem();
  it->d.item = item;
  return it;
}

}  // namespace

void accessory_can_begin(ElcChannels *ch) { s_ch = ch; }

void accessory_can_transfer(const ElcChTransfer &t, uint32_t now_ms) {
  s_now = now_ms;
  if (t.base == ELC_CH_NET && t.op() == ELC_NET_ANNOUNCE) {
    ElcNetAnnounce a;
    AccNode *n = find(t.src);
    if (n && elc_net_decode_announce(t.data, t.len, a) && a.generation != n->generation) {
      // its descriptions changed: forget them (removed items go) and ask again
      n->generation = a.generation;
      for (int i = 0; i < n->n; i++) n->items[i].defined = false;
      n->sig = mix(n->sig, 0xA11);
      s_query_ms[t.src & 31] = 0;
      if (s_ch) {
        s_ch->query(t.src, ELC_NET_Q_UI);
        s_query_ms[t.src & 31] = now_ms ? now_ms : 1;
      }
    }
    return;
  }
  if (t.base != ELC_CH_UI) return;
  const uint8_t op = t.op();
  if (op == ELC_UI_ITEM) {
    ElcUiItem d;
    if (!elc_ui_decode(t, d)) return;
    AccNode *n = find_or_add(t.src);
    if (!n) return;
    if (s_ch && s_ch->nodeInfo(t.src).announced) n->generation = s_ch->nodeInfo(t.src).a.generation;
    AccItem *it = item_of(n, d.item, true);
    if (!it) return;
    it->d = d;
    it->defined = true;
    s_rx_items++;
    n->sig = mix(mix(n->sig, d.item), 0x17E);
  } else if (op == ELC_UI_VALUE || op == ELC_UI_TEXT || op == ELC_UI_RESULT) {
    AccNode *n = find_or_add(t.src);
    if (!n) return;
    if (op == ELC_UI_VALUE) {
      ElcUiValue v;
      if (!elc_ui_decode(t, v)) return;
      AccItem *it = item_of(n, v.item, true);
      if (!it) return;
      if (!it->has_value || it->value != v.value) n->sig = mix(mix(n->sig, v.item), (uint32_t)v.value);
      it->has_value = true;
      it->value = v.value;
      s_rx_values++;
    } else if (op == ELC_UI_TEXT) {
      ElcUiText v;
      if (!elc_ui_decode(t, v)) return;
      AccItem *it = item_of(n, v.item, true);
      if (!it) return;
      if (!it->has_text || strcmp(it->text, v.text) != 0) n->sig = mix(mix(n->sig, v.item), 0x7E47 + (uint32_t)strlen(v.text));
      it->has_text = true;
      memcpy(it->text, v.text, sizeof(it->text));
      s_rx_values++;
    } else {
      ElcUiResult r;
      if (!elc_ui_decode(t, r)) return;
      AccItem *it = item_of(n, r.item, false);
      if (!it || !s_ch || r.asker != s_ch->node()) return;  // someone else's SET
      it->pending = false;
      it->result = r.result;
      it->result_ms = now_ms ? now_ms : 1;
      n->sig = mix(mix(n->sig, r.item), 0x4E5 + r.result);
    }
  }
}

void accessory_can_poll(uint32_t now_ms) {
  s_now = now_ms;
  if (!s_ch) return;
  // Ask UI producers we have no items from (a screen that booted after them, or a missed burst): announced ones
  // that say they produce UI, and any node we heard UI values from.
  for (uint8_t node = 1; node < 32; node++) {
    const ElcChannels::Node &nd = s_ch->nodeInfo(node);
    const AccNode *n = find(node);
    const bool producer = nd.announced && (nd.a.produces & elc_ch_bit(ELC_CH_UI));
    if ((!producer && !n) || node == s_ch->node()) continue;
    bool have = false;
    for (int i = 0; n && i < n->n && !have; i++) have = n->items[i].defined;
    if (have) continue;
    if (s_query_ms[node] && now_ms - s_query_ms[node] < kQueryMs) continue;
    s_query_ms[node] = now_ms ? now_ms : 1;
    s_ch->query(node, nd.announced ? ELC_NET_Q_UI : (uint8_t)(ELC_NET_Q_UI | ELC_NET_Q_ANNOUNCE));
  }
  // VIEW keepalive for the page on screen; one VIEW 0xFF when it closes.
  if (s_view_node) {
    if (!s_view_ms || now_ms - s_view_ms >= kViewMs) {
      s_view_ms = now_ms ? now_ms : 1;
      ElcChWriter w;
      elc_ui_encode(ElcUiView{s_view_node, s_view_group == ELC_UI_TOP ? ELC_UI_VIEW_TOP : s_view_group}, w);
      s_ch->send(ELC_CH_UI, w);
    }
  }
  // SETs nobody answered
  for (AccNode &n : s_nodes) {
    for (int i = 0; n.node && i < n.n; i++) {
      AccItem &it = n.items[i];
      if (it.pending && now_ms - it.pending_ms > kPendingMs) {
        it.pending = false;
        it.result = 0xFF;  // no answer
        it.result_ms = now_ms ? now_ms : 1;
        n.sig = mix(n.sig, 0xDEAD);
      }
    }
  }
}

int accessory_can_nodes(uint8_t *out, int max) {
  int k = 0;
  for (uint8_t node = 1; node < 32 && k < max; node++) {
    const AccNode *n = find(node);
    if (!n) continue;
    bool any = false;
    for (int i = 0; i < n->n && !any; i++) any = n->items[i].defined;
    if (any) out[k++] = node;
  }
  return k;
}

const char *accessory_can_name(uint8_t node) {
  if (s_ch && s_ch->nodeInfo(node).announced && s_ch->nodeInfo(node).a.name[0]) return s_ch->nodeInfo(node).a.name;
  static char buf[16];
  snprintf(buf, sizeof(buf), "Node 0x%02X", (unsigned)node);
  return buf;
}

int accessory_can_items(uint8_t node, uint8_t parent, bool advanced, const AccItem **out, int max) {
  const AccNode *n = find(node);
  if (!n) return 0;
  int k = 0;
  // in item-number order
  for (int num = 0; num < 255 && k < max; num++) {
    for (int i = 0; i < n->n; i++) {
      const AccItem &it = n->items[i];
      if (it.d.item != num || !it.defined || it.d.parent != parent) continue;
      if ((it.d.flags & ELC_UI_F_ADVANCED) && !advanced) continue;
      out[k++] = &it;
    }
  }
  return k;
}

const AccItem *accessory_can_item(uint8_t node, uint8_t item) {
  AccNode *n = find(node);
  return n ? item_of(n, item, false) : nullptr;
}

void accessory_can_value_text(const AccItem &it, char *out, size_t n) {
  if (!n) return;
  out[0] = 0;
  switch (it.d.kind) {
    case ELC_UI_GROUP: snprintf(out, n, ">"); return;
    case ELC_UI_ACTION: return;
    case ELC_UI_TEXTOUT: snprintf(out, n, "%s", it.has_text && it.text[0] ? it.text : "--"); return;
    default: break;
  }
  if (!it.has_value) {
    snprintf(out, n, "--");
    return;
  }
  if (it.d.kind == ELC_UI_TOGGLE) {
    snprintf(out, n, "%s", it.value ? "On" : "Off");
  } else if (it.d.kind == ELC_UI_CHOICE) {
    if (!it.d.option((int)it.value, out, n)) snprintf(out, n, "%ld", (long)it.value);
  } else {
    char v[24];
    elc_ui_format(it.value, it.d.exponent, v, sizeof(v));
    snprintf(out, n, it.d.unit[0] ? "%s %s" : "%s", v, it.d.unit);
  }
}

bool accessory_can_set(uint8_t node, uint8_t item, int32_t value) {
  if (!s_ch) return false;
  AccNode *n = find(node);
  AccItem *it = n ? item_of(n, item, false) : nullptr;
  if (!it || !it->defined || (it->d.flags & ELC_UI_F_RO)) return false;
  ElcChWriter w;
  elc_ui_encode(ElcUiSet{node, item, value}, w);
  if (!s_ch->send(ELC_CH_UI, w)) return false;
  s_tx_sets++;
  it->pending = true;
  it->pending_ms = s_now;
  n->sig = mix(n->sig, 0x5E7 + item);
  return true;
}

void accessory_can_view(uint8_t node, uint8_t group) {
  if (node == s_view_node && group == s_view_group) return;
  if (!node && s_view_node && s_ch) {  // the page closed: say so once
    ElcChWriter w;
    elc_ui_encode(ElcUiView{s_view_node, ELC_UI_VIEW_CLOSED}, w);
    s_ch->send(ELC_CH_UI, w);
  }
  s_view_node = node;
  s_view_group = group;
  s_view_ms = 0;  // send at the next poll
}

uint32_t accessory_can_signature(uint8_t node) {
  const AccNode *n = find(node);
  return n ? n->sig : 0;
}

void accessory_can_describe(char *out, size_t n) {
  int w = snprintf(out, n, "accessories: items %lu, values %lu, sets %lu;", (unsigned long)s_rx_items,
                   (unsigned long)s_rx_values, (unsigned long)s_tx_sets);
  for (const AccNode &x : s_nodes) {
    if (!x.node || w <= 0 || (size_t)w >= n) continue;
    int defined = 0;
    for (int i = 0; i < x.n; i++) defined += x.items[i].defined;
    w += snprintf(out + w, n - (size_t)w, " 0x%02X %s (%d items)", (unsigned)x.node, accessory_can_name(x.node), defined);
  }
}
