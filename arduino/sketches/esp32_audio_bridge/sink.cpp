// Only with the CAN library (the PlatformIO build); the plain arduino-cli build has no sink.
#if __has_include(<elc_chan.h>)

#include "sink.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "audio_dsp.h"

namespace {

constexpr uint32_t kStatusMs = 1000;
constexpr uint32_t kHolderLostMs = 3000;   // RET is freed when its holder's heartbeat has been gone this long
constexpr uint32_t kRadioWaitMs = 6000;    // booted to the radio and none heard by then: Bluetooth
constexpr uint32_t kSaveQuietMs = 2000;
constexpr uint32_t kViewMs = 3000;
constexpr uint16_t kRate10 = 4800;         // the rail runs at 48 kHz

const char *src_name(uint8_t s) {
  switch (s) {
    case ELC_SRC_BLUETOOTH: return "Bluetooth";
    case ELC_SRC_RADIO: return "radio";
    case ELC_SRC_AUX: return "aux in";
    default: return "off";
  }
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

void AudioSink::begin(ElcChannels *ch, SinkIo *io, const Caps &caps, const SinkSettings &saved, uint32_t now_ms) {
  _ch = ch;
  _io = io;
  _caps = caps;
  _s = saved;
  _s.volume = (uint8_t)clampi(_s.volume, 0, 15);
  _s.chime_volume = (uint8_t)clampi(_s.chime_volume, 1, 15);
  _s.fader = (int8_t)clampi(_s.fader, -10, 10);
  _s.balance = (int8_t)clampi(_s.balance, -10, 10);
  if (_s.eq_preset > ELC_EQ_PRESET_CUSTOM) _s.eq_preset = ELC_EQ_PRESET_FLAT;
  for (auto &b : _s.eq_custom) b = (int8_t)clampi(b, -ELC_EQ_MAX_DB, ELC_EQ_MAX_DB);
  if (_s.source == ELC_SRC_AUX && !caps.aux) _s.source = ELC_SRC_BLUETOOTH;
  if (_s.source > ELC_SRC_AUX) _s.source = ELC_SRC_BLUETOOTH;
  _boot_ms = now_ms ? now_ms : 1;
  _radio_wait = _s.source == ELC_SRC_RADIO;  // the radio card may announce after us
  if (_ch) _ch->setFactoryDefaults(_s.factory);
  if (_io) _io->apply();
}

uint8_t AudioSink::radio() const { return _ch ? _ch->findNode(ELC_TYPE_RADIO) : 0; }

SinkMusic AudioSink::music() const {
  switch (_s.source) {
    case ELC_SRC_BLUETOOTH: return SinkMusic::Bluetooth;
    case ELC_SRC_RADIO: return SinkMusic::Rail;
    case ELC_SRC_AUX: return SinkMusic::Aux;
    default: return SinkMusic::Off;
  }
}

void AudioSink::eq_db(int8_t out[ELC_EQ_BANDS]) const {
  const int8_t *src = _s.eq_preset == ELC_EQ_PRESET_CUSTOM ? _s.eq_custom : dsp::Eq10::kPresetDb[_s.eq_preset];
  memcpy(out, src, ELC_EQ_BANDS);
}

void AudioSink::logf(const char *fmt, ...) {
  if (!_io) return;
  char line[120];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  _io->log(line);
}

void AudioSink::changed(uint32_t now_ms, bool setting) {
  if (setting) {
    if (_s.factory && _ch) _ch->setFactoryDefaults(false);
    _s.factory = false;
  }
  _dirty_ms = now_ms ? now_ms : 1;
  if (_io) _io->apply();
}

// ---- sources ----

void AudioSink::set_source(uint8_t code, uint32_t now_ms, const char *why) {
  _radio_wait = false;
  if (code == ELC_SRC_RADIO && !radio()) {
    logf("source radio asked for (%s), but no radio card heard: stays %s", why, src_name(_s.source));
    send_status(now_ms);
    return;
  }
  if (code == ELC_SRC_AUX && !_caps.aux) {
    logf("source aux asked for (%s), but this card has no aux input", why);
    send_status(now_ms);
    return;
  }
  if (code != _s.source) {
    if (_s.source == ELC_SRC_BLUETOOTH) {  // leaving Bluetooth: pause it, and play it again when it comes back
      _bt_was_playing = _io && _io->bt_playing();
      if (_bt_was_playing) _io->bt_media(ELC_MEDIA_PAUSE);
    } else if (code == ELC_SRC_BLUETOOTH && _bt_was_playing && _io) {
      _io->bt_media(ELC_MEDIA_PLAY);
    }
    logf("source %s (%s)%s", src_name(code), why,
         code != ELC_SRC_BLUETOOTH && _bt_was_playing ? ", Bluetooth paused"
                                                     : (code == ELC_SRC_BLUETOOTH && _bt_was_playing ? ", Bluetooth plays" : ""));
    _s.source = code;
    changed(now_ms, false);
  }
  send_status(now_ms);  // the sources learn from STATUS which one plays; the asker sees its echo
}

/** The next one round Bluetooth, the radio (when heard), aux (when fitted) and off. */
void AudioSink::source_next(uint32_t now_ms) {
  uint8_t list[4];
  int n = 0;
  list[n++] = ELC_SRC_BLUETOOTH;
  if (radio()) list[n++] = ELC_SRC_RADIO;
  if (_caps.aux) list[n++] = ELC_SRC_AUX;
  list[n++] = ELC_SRC_OFF;
  int i = n - 1;
  for (int k = 0; k < n; k++) {
    if (list[k] == _s.source) i = k;
  }
  set_source(list[(i + 1) % n], now_ms, "next");
}

// ---- volume and media ----

void AudioSink::set_volume15(uint8_t v, uint32_t now_ms) {
  v = (uint8_t)clampi(v, 0, 15);
  if (v == _s.volume && !_muted) return;
  _s.volume = v;
  _muted = false;
  changed(now_ms, false);
}

void AudioSink::volume_cmd(const ElcAudioVolume &v, uint32_t now_ms) {
  const int cur100 = (_s.volume * 100 + 7) / 15;
  switch (v.action) {
    case 0: set_volume15((uint8_t)((clampi(v.value, 0, 100) * 15 + 50) / 100), now_ms); break;
    case 1: {
      int to = (clampi(cur100 + v.value, 0, 100) * 15 + 50) / 100;
      if (v.value > 0 && to <= _s.volume) to = _s.volume + 1;  // a step always moves at least one notch
      if (v.value < 0 && to >= _s.volume) to = _s.volume - 1;
      set_volume15((uint8_t)clampi(to, 0, 15), now_ms);
      break;
    }
    case 2:
      _muted = !_muted;
      logf("%s", _muted ? "muted" : "unmuted");
      changed(now_ms, false);
      break;
    default: break;
  }
}

void AudioSink::media(uint8_t action, uint8_t value) {
  switch (_s.source) {
    case ELC_SRC_RADIO: {
      const uint8_t r = radio();
      if (!r) return;
      ElcChWriter w;
      elc_audio_encode(ElcAudioMedia{r, action, value}, w);
      _io->send(ELC_CH_AUDIO_CMD, w);  // the radio acts on it
      return;
    }
    case ELC_SRC_BLUETOOTH:
      if (action == ELC_MEDIA_TUNE_UP) action = ELC_MEDIA_NEXT;  // Bluetooth: tuning steps tracks
      if (action == ELC_MEDIA_TUNE_DOWN) action = ELC_MEDIA_PREV;
      if (action == ELC_MEDIA_TOGGLE || action == ELC_MEDIA_PLAY || action == ELC_MEDIA_PAUSE ||
          action == ELC_MEDIA_NEXT || action == ELC_MEDIA_PREV) {
        _io->bt_media(action);
      }
      return;
    default: return;  // aux and off: nothing to tell
  }
}

// ---- the rail ----

uint8_t AudioSink::rank(uint8_t node) const {
  if (!_ch) return 1;
  const ElcChannels::Node &n = _ch->nodeInfo(node);
  if (!n.announced) return 1;
  if (n.a.type == ELC_TYPE_VOICE) return 2;
  if (n.a.type == ELC_TYPE_RADIO) return 0;
  return 1;
}

void AudioSink::rail_send(uint8_t dest, uint8_t state) {
  ElcNetRail r;
  r.dest = dest;
  r.slot = _caps.slot;
  r.state = state;
  r.rate10 = kRate10;
  uint8_t buf[8];
  const size_t n = elc_net_encode_rail(r, buf, sizeof(buf));
  ElcChWriter w;
  w.bytes(buf, n);
  _io->send(ELC_CH_NET, w);
}

void AudioSink::rail(const ElcNetRail &r, uint8_t src, uint32_t now_ms) {
  if (r.state == 0) {  // a request
    if (!_holder || _holder == src) {
      _holder = src;
    } else if (rank(src) > rank(_holder)) {
      logf("RET withdrawn from 0x%02X for 0x%02X", (unsigned)_holder, (unsigned)src);
      rail_send(_holder, 2);
      _holder = src;
    } else {
      logf("RET denied to 0x%02X: 0x%02X holds it", (unsigned)src, (unsigned)_holder);
      rail_send(src, 2);
      return;
    }
    _holder_ms = now_ms ? now_ms : 1;
    rail_send(src, 1);
    logf("RET granted to 0x%02X (%s)", (unsigned)src, rail_voice() ? "a voice: the music ducks" : "the radio");
    changed(now_ms, false);
    send_status(now_ms);
  } else if (r.state == 2 && src == _holder) {
    logf("RET released by 0x%02X", (unsigned)src);
    _holder = 0;
    changed(now_ms, false);
    send_status(now_ms);
  }
}

// ---- STATUS ----

void AudioSink::status_tuple(uint8_t out[7]) const {
  bool playing = false;
  uint8_t node = 0;
  switch (_s.source) {
    case ELC_SRC_BLUETOOTH:
      playing = _io && _io->bt_playing();
      node = _ch ? _ch->node() : 0;
      break;
    case ELC_SRC_RADIO:
      node = radio();
      playing = node && _holder == node;
      break;
    case ELC_SRC_AUX:
      playing = true;
      node = _ch ? _ch->node() : 0;
      break;
    default: break;
  }
  const bool ducked = (_io && _io->chimes_ducking()) || rail_voice();
  out[0] = (uint8_t)((_s.volume * 100 + 7) / 15);
  out[1] = (uint8_t)((_muted ? ELC_AUDIO_F_MUTED : 0) | (playing ? ELC_AUDIO_F_PLAYING : 0) |
                     (ducked ? ELC_AUDIO_F_DUCKED : 0) | (_caps.aux ? ELC_AUDIO_F_AUX : 0));
  out[2] = _io ? _io->chimes_active() : 0;
  out[3] = 0;  // no AUDIO_STREAM yet
  out[4] = _caps.rear ? 0x0F : 0x03;
  out[5] = _s.source;
  out[6] = node;
}

void AudioSink::send_status(uint32_t now_ms) {
  if (!_io) return;
  uint8_t t[7];
  status_tuple(t);
  ElcAudioStatus st;
  st.volume = t[0];
  st.flags = t[1];
  st.active = t[2];
  st.stream_owner = t[3];
  st.zones = t[4];
  st.source = t[5];
  st.source_node = t[6];
  ElcChWriter w;
  elc_audio_encode(st, w);
  _io->send(ELC_CH_AUDIO_CMD, w);
  memcpy(_sent, t, sizeof(_sent));
  _sent_any = true;
  _status_ms = now_ms;
}

// ---- the page ----

void AudioSink::describe_ui() {
  if (!_io) return;
  auto item = [&](uint8_t n, uint8_t parent, uint8_t kind, int32_t lo, int32_t hi, const char *label, const char *unit,
                  const char *options) {
    ElcUiItem it;
    it.item = n;
    it.parent = parent;
    it.kind = kind;
    it.flags = kind == ELC_UI_GROUP ? 0 : ELC_UI_F_SETTING;
    it.min = lo;
    it.max = hi;
    it.step = kind == ELC_UI_NUMBER ? 1 : 0;
    snprintf(it.label, sizeof(it.label), "%s", label);
    snprintf(it.unit, sizeof(it.unit), "%s", unit);
    snprintf(it.options, sizeof(it.options), "%s", options);
    ElcChWriter w;
    elc_ui_encode(it, w);
    _io->send(ELC_CH_UI, w);
  };
  item(ELC_SINK_ITEM_CHIME_VOL, ELC_UI_TOP, ELC_UI_NUMBER, 1, 15, "Chime volume", "", "");
  if (_caps.rear) item(ELC_SINK_ITEM_FADER, ELC_UI_TOP, ELC_UI_NUMBER, -10, 10, "Fader", "R-F", "");
  item(ELC_SINK_ITEM_BALANCE, ELC_UI_TOP, ELC_UI_NUMBER, -10, 10, "Balance", "L-R", "");
  item(ELC_SINK_ITEM_EQ, ELC_UI_TOP, ELC_UI_GROUP, 0, 0, "Equalizer", "", "");
  item(ELC_SINK_ITEM_EQ_PRESET, ELC_SINK_ITEM_EQ, ELC_UI_CHOICE, 0, ELC_EQ_PRESET_CUSTOM, "Preset", "", ELC_EQ_PRESETS);
  for (int b = 0; b < ELC_EQ_BANDS; b++) {
    char label[12];
    const char *f = elc_eq_band_label(b);
    const int len = (int)strlen(f);
    if (f[len - 1] == 'k') snprintf(label, sizeof(label), "%.*s kHz", len - 1, f);  // "16k" -> "16 kHz"
    else snprintf(label, sizeof(label), "%s Hz", f);
    item((uint8_t)(ELC_SINK_ITEM_EQ_BAND0 + b), ELC_SINK_ITEM_EQ, ELC_UI_NUMBER, -ELC_EQ_MAX_DB, ELC_EQ_MAX_DB, label,
         "dB", "");
  }
  send_values(ELC_UI_TOP);
  send_values(ELC_SINK_ITEM_EQ);
}

void AudioSink::ui_value(uint8_t item) {
  int32_t v = 0;
  if (item == ELC_SINK_ITEM_CHIME_VOL) v = _s.chime_volume;
  else if (item == ELC_SINK_ITEM_FADER) v = _s.fader;
  else if (item == ELC_SINK_ITEM_BALANCE) v = _s.balance;
  else if (item == ELC_SINK_ITEM_EQ_PRESET) v = _s.eq_preset;
  else if (item >= ELC_SINK_ITEM_EQ_BAND0 && item < ELC_SINK_ITEM_EQ_BAND0 + ELC_EQ_BANDS) {
    int8_t db[ELC_EQ_BANDS];
    eq_db(db);
    v = db[item - ELC_SINK_ITEM_EQ_BAND0];
  } else {
    return;
  }
  ElcChWriter w;
  elc_ui_encode(ElcUiValue{item, v}, w);
  _io->send(ELC_CH_UI, w);
}

void AudioSink::send_values(uint8_t group) {
  if (group == ELC_SINK_ITEM_EQ) {
    for (uint8_t i = ELC_SINK_ITEM_EQ_PRESET; i < ELC_SINK_ITEM_EQ_BAND0 + ELC_EQ_BANDS; i++) ui_value(i);
  } else {
    ui_value(ELC_SINK_ITEM_CHIME_VOL);
    if (_caps.rear) ui_value(ELC_SINK_ITEM_FADER);
    ui_value(ELC_SINK_ITEM_BALANCE);
  }
}

void AudioSink::ui_set(const ElcUiSet &set, uint8_t asker, uint32_t now_ms) {
  uint8_t r = ELC_UI_OK;
  const int32_t v = set.value;
  const uint8_t it = set.item;
  bool eq = false;
  if (it == ELC_SINK_ITEM_CHIME_VOL) {
    if (v < 1 || v > 15) {
      r = ELC_UI_RANGE;
    } else {
      _s.chime_volume = (uint8_t)v;
      _io->chime_preview(_s.chime_volume);
      changed(now_ms, true);
    }
  } else if (it == ELC_SINK_ITEM_FADER || it == ELC_SINK_ITEM_BALANCE) {
    if (it == ELC_SINK_ITEM_FADER && !_caps.rear) {
      r = ELC_UI_UNKNOWN;
    } else if (v < -10 || v > 10) {
      r = ELC_UI_RANGE;
    } else {
      (it == ELC_SINK_ITEM_FADER ? _s.fader : _s.balance) = (int8_t)v;
      changed(now_ms, true);
    }
  } else if (it == ELC_SINK_ITEM_EQ_PRESET) {
    if (v < 0 || v > ELC_EQ_PRESET_CUSTOM) {
      r = ELC_UI_RANGE;
    } else {
      _s.eq_preset = (uint8_t)v;
      eq = true;
      changed(now_ms, true);
    }
  } else if (it >= ELC_SINK_ITEM_EQ_BAND0 && it < ELC_SINK_ITEM_EQ_BAND0 + ELC_EQ_BANDS) {
    if (v < -ELC_EQ_MAX_DB || v > ELC_EQ_MAX_DB) {
      r = ELC_UI_RANGE;
    } else {
      // A band moved on a preset: the preset's curve becomes the Custom one, with this band changed.
      if (_s.eq_preset != ELC_EQ_PRESET_CUSTOM) {
        memcpy(_s.eq_custom, dsp::Eq10::kPresetDb[_s.eq_preset], ELC_EQ_BANDS);
        _s.eq_preset = ELC_EQ_PRESET_CUSTOM;
      }
      _s.eq_custom[it - ELC_SINK_ITEM_EQ_BAND0] = (int8_t)v;
      eq = true;
      changed(now_ms, true);
    }
  } else {
    r = ELC_UI_UNKNOWN;
  }
  ElcChWriter w;
  elc_ui_encode(ElcUiResult{it, r, asker}, w);
  _io->send(ELC_CH_UI, w);
  if (eq) send_values(ELC_SINK_ITEM_EQ);  // the preset and every band, whatever moved
  else if (r != ELC_UI_UNKNOWN) ui_value(it);
}

// ---- in ----

void AudioSink::transfer(const ElcChTransfer &t, uint32_t now_ms) {
  if (!_ch || !_io || !t.for_me(_ch->node())) return;
  if (t.base == ELC_CH_AUDIO_CMD) {
    ElcAudioVolume v;
    ElcAudioMedia m;
    ElcAudioSource s;
    if (elc_audio_decode(t, v)) {
      volume_cmd(v, now_ms);
    } else if (elc_audio_decode(t, m)) {
      media(m.action, m.value);
    } else if (elc_audio_decode(t, s)) {
      if (s.source == 0) source_next(now_ms);
      else if (s.source == ELC_SRC_REQ_OFF) set_source(ELC_SRC_OFF, now_ms, "asked");
      else if (s.source <= ELC_SRC_AUX) set_source(s.source, now_ms, "asked");
    }
  } else if (t.base == ELC_CH_NET && t.op() == ELC_NET_RAIL) {
    ElcNetRail r;
    if (elc_net_decode_rail(t.data, t.len, r) && r.dest == _ch->node()) rail(r, t.src, now_ms);
  } else if (t.base == ELC_CH_UI) {
    ElcUiSet set;
    ElcUiView view;
    if (elc_ui_decode(t, set)) {
      ui_set(set, t.src, now_ms);
    } else if (elc_ui_decode(t, view)) {
      _view = view.item;
      _view_ms = now_ms;
      if (view.item != ELC_UI_VIEW_CLOSED) send_values(view.item == ELC_SINK_ITEM_EQ ? ELC_SINK_ITEM_EQ : ELC_UI_TOP);
    }
  }
}

void AudioSink::tick(uint32_t now_ms) {
  if (!_ch || !_io) return;
  if (_radio_wait) {
    if (radio()) {
      _radio_wait = false;  // it has announced: STATUS names it now
    } else if (now_ms - _boot_ms > kRadioWaitMs) {
      _radio_wait = false;
      set_source(ELC_SRC_BLUETOOTH, now_ms, "no radio card heard since boot");
    }
  }
  if (_holder && !_io->node_alive(_holder) && now_ms - _holder_ms > kHolderLostMs) {
    logf("RET freed: 0x%02X's heartbeat stopped", (unsigned)_holder);
    _holder = 0;
    changed(now_ms, false);
  }
  uint8_t t[7];
  status_tuple(t);
  if (!_sent_any || now_ms - _status_ms >= kStatusMs || memcmp(t, _sent, sizeof(t)) != 0) send_status(now_ms);
  if (_dirty_ms && now_ms - _dirty_ms >= kSaveQuietMs) {
    _dirty_ms = 0;
    _io->save(_s);
  }
}

void AudioSink::describe(char *out, size_t n) const {
  int8_t db[ELC_EQ_BANDS];
  eq_db(db);
  char eq[48];
  int k = 0;
  for (int b = 0; b < ELC_EQ_BANDS && k < (int)sizeof(eq) - 5; b++) k += snprintf(eq + k, sizeof(eq) - k, "%+d ", db[b]);
  char preset[12] = "?";
  {
    const char *p = ELC_EQ_PRESETS;
    for (int i = 0; i < _s.eq_preset && p; i++) {
      p = strchr(p, '\n');
      if (p) p++;
    }
    if (p) {
      const char *e = strchr(p, '\n');
      snprintf(preset, sizeof(preset), "%.*s", e ? (int)(e - p) : (int)strlen(p), p);
    }
  }
  snprintf(out, n, "sink: source %s (node 0x%02X), volume %u/15%s, RET %s0x%02X, EQ %s [%s], fader %+d balance %+d, chimes %u",
           src_name(_s.source), (unsigned)(_s.source == ELC_SRC_RADIO ? radio() : (_ch ? _ch->node() : 0)),
           (unsigned)_s.volume, _muted ? " muted" : "", rail_voice() ? "voice " : "", (unsigned)_holder, preset, eq,
           _s.fader, _s.balance, (unsigned)_s.chime_volume);
}

#endif
