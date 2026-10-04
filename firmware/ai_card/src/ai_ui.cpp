#include "ai_ui.h"

#include <string.h>

namespace {
struct Def {
  uint8_t kind, flags;
  int32_t min, max, step;
  const char *label, *unit, *options;
};
const Def kDefs[kUiCount] = {
    {ELC_UI_TOGGLE, ELC_UI_F_SETTING, 0, 1, 1, "Assistant", "", ""},
    {ELC_UI_TOGGLE, ELC_UI_F_SETTING, 0, 1, 1, "Wake word", "", ""},
    {ELC_UI_CHOICE, ELC_UI_F_SETTING, 0, 2, 1, "Online lookups", "", "Auto\nAlways\nOffline only"},
    {ELC_UI_TOGGLE, ELC_UI_F_SETTING, 0, 1, 1, "Mute microphone", "", ""},
    {ELC_UI_TOGGLE, ELC_UI_F_SETTING, 0, 1, 1, "Speak answers", "", ""},
    {ELC_UI_NUMBER, ELC_UI_F_SETTING, 0, 100, 5, "Voice level", "%", ""},
    {ELC_UI_TEXTOUT, ELC_UI_F_RO | ELC_UI_F_LIVE, 0, 0, 0, "Status", "", ""},
    {ELC_UI_TEXTOUT, ELC_UI_F_RO, 0, 0, 0, "Heard", "", ""},
    {ELC_UI_TEXTOUT, ELC_UI_F_RO, 0, 0, 0, "Answer", "", ""},
    {ELC_UI_TEXTOUT, ELC_UI_F_RO | ELC_UI_F_LIVE, 0, 0, 0, "Online", "", ""},
    {ELC_UI_TEXTOUT, ELC_UI_F_RO | ELC_UI_F_LIVE, 0, 0, 0, "Compute module", "", ""},
    {ELC_UI_ACTION, ELC_UI_F_CONFIRM, 0, 1, 1, "Forget conversation", "", ""},
};
}  // namespace

int32_t AiUi::value(uint8_t item) const {
  switch (item) {
    case kUiEnabled: return _s.enabled;
    case kUiWake: return _s.wake_word;
    case kUiOnline: return _s.online;
    case kUiMute: return _s.mic_muted;
    case kUiSpeak: return _s.speak;
    case kUiLevel: return _s.voice_level;
    default: return 0;
  }
}

void AiUi::describe() {
  for (uint8_t i = 0; i < kUiCount; i++) {
    const Def &d = kDefs[i];
    ElcUiItem it;
    it.item = i;
    it.parent = ELC_UI_TOP;
    it.kind = d.kind;
    it.flags = d.flags;
    it.min = d.min;
    it.max = d.max;
    it.step = d.step;
    elc_ch_copy_text(it.label, sizeof(it.label), d.label);
    elc_ch_copy_text(it.unit, sizeof(it.unit), d.unit);
    elc_ch_copy_text(it.options, sizeof(it.options), d.options);
    ElcChWriter w;
    elc_ui_encode(it, w);
    emit(w);
  }
}

void AiUi::sendValue(uint8_t item) {
  if (item >= kUiCount) return;
  ElcChWriter w;
  if (kDefs[item].kind == ELC_UI_TEXTOUT) {
    ElcUiText t;
    t.item = item;
    elc_ch_copy_text(t.text, sizeof(t.text), _text[item]);
    elc_ui_encode(t, w);
  } else if (kDefs[item].kind == ELC_UI_ACTION) {
    return;
  } else {
    ElcUiValue v;
    v.item = item;
    v.value = value(item);
    elc_ui_encode(v, w);
  }
  emit(w);
}

void AiUi::sendAll() {
  for (uint8_t i = 0; i < kUiCount; i++) sendValue(i);
}

uint8_t AiUi::onSet(const ElcUiSet &set, uint8_t asker) {
  uint8_t result = ELC_UI_OK;
  if (set.item >= kUiCount) {
    result = ELC_UI_UNKNOWN;
  } else {
    const Def &d = kDefs[set.item];
    if (d.flags & ELC_UI_F_RO) {
      result = ELC_UI_READONLY;
    } else if (set.value < d.min || set.value > d.max || (d.kind == ELC_UI_NUMBER && (set.value - d.min) % d.step)) {
      result = ELC_UI_RANGE;
    } else {
      const int32_t before = value(set.item);
      switch (set.item) {
        case kUiEnabled: _s.enabled = set.value != 0; break;
        case kUiWake: _s.wake_word = set.value != 0; break;
        case kUiOnline: _s.online = (uint8_t)set.value; break;
        case kUiMute: _s.mic_muted = set.value != 0; break;
        case kUiSpeak: _s.speak = set.value != 0; break;
        case kUiLevel: _s.voice_level = (uint8_t)set.value; break;
        case kUiForget: if (set.value) _forget = true; break;
      }
      if (d.flags & ELC_UI_F_SETTING) {
        if (value(set.item) != before) _dirty = true;
      }
    }
  }
  if (set.item < kUiCount) sendValue(set.item);  // the menu shows the value from VALUE, never the one it asked for
  ElcUiResult r;
  r.item = set.item;
  r.result = result;
  r.asker = asker;
  ElcChWriter w;
  elc_ui_encode(r, w);
  emit(w);
  return result;
}

void AiUi::setText(uint8_t item, const char *text) {
  if (item >= kUiCount || kDefs[item].kind != ELC_UI_TEXTOUT) return;
  char t[65];
  elc_ch_copy_text(t, sizeof(t), text ? text : "");
  if (strcmp(t, _text[item]) == 0) return;
  memcpy(_text[item], t, sizeof(t));
  sendValue(item);
}
