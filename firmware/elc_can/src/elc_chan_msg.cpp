#include "elc_chan_msg.h"

#include <stdio.h>
#include <string.h>

// A reader positioned after the opcode, when the transfer is on `base` with opcode `op`.
static bool open(const ElcChTransfer &t, uint16_t base, uint8_t op, ElcChReader &r) {
  if (t.base != base || t.len < 1 || t.data[0] != op) return false;
  r = ElcChReader(t.data + 1, t.len - 1);
  return true;
}

static void copy(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
  dst[i] = 0;
}

// ---- SYSTEM

void elc_sys_encode(const ElcSysTime &v, ElcChWriter &w) { w.u8(ELC_SYS_TIME).u32(v.utc).s8(v.offset_q).u8(v.source); }
void elc_sys_encode(const ElcSysVehicle &v, ElcChWriter &w) {
  w.u8(ELC_SYS_VEHICLE).u16(v.flags).u8(v.backlight).u16(v.speed_x10);
}
void elc_sys_encode(const ElcSysPrefs &v, ElcChWriter &w) { w.u8(ELC_SYS_PREFS).u8(v.units).u8(v.theme).u8(v.clock24); }
void elc_sys_encode(const ElcSysPower &v, ElcChWriter &w) { w.u8(ELC_SYS_POWER).u8(v.event).u8(v.seconds); }

bool elc_sys_decode(const ElcChTransfer &t, ElcSysTime &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_SYSTEM, ELC_SYS_TIME, r)) return false;
  v.utc = r.u32();
  v.offset_q = r.s8();
  v.source = r.u8();
  return r.ok();
}
bool elc_sys_decode(const ElcChTransfer &t, ElcSysVehicle &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_SYSTEM, ELC_SYS_VEHICLE, r)) return false;
  v.flags = r.u16();
  v.backlight = r.u8();
  v.speed_x10 = r.u16();
  return r.ok();
}
bool elc_sys_decode(const ElcChTransfer &t, ElcSysPrefs &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_SYSTEM, ELC_SYS_PREFS, r)) return false;
  v.units = r.u8();
  v.theme = r.u8();
  v.clock24 = r.u8();
  return r.ok();
}
bool elc_sys_decode(const ElcChTransfer &t, ElcSysPower &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_SYSTEM, ELC_SYS_POWER, r)) return false;
  v.event = r.u8();
  v.seconds = r.u8();
  return r.ok();
}

// ---- LOG

void elc_log_encode(const ElcLogLine &v, ElcChWriter &w) { w.u8(ELC_LOG_LINE).u8(v.level).text(v.text, 120); }
bool elc_log_decode(const ElcChTransfer &t, ElcLogLine &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_LOG, ELC_LOG_LINE, r)) return false;
  v.level = r.u8();
  if (!r.ok()) return false;
  r.rest_text(v.text, sizeof(v.text));
  return true;
}

// ---- SENSOR

void elc_sensor_encode(const ElcSensorDefine &v, ElcChWriter &w) {
  w.u8(ELC_SENSOR_DEFINE).u8(v.sensor).u8(v.quantity).s8(v.exponent).u8(v.flags).u16(v.period_ms).s32(v.disp_min)
      .s32(v.disp_max).s32(v.caution_lo).s32(v.caution_hi).s32(v.warn_lo).s32(v.warn_hi).text(v.name, 20);
}
void elc_sensor_encode(const ElcSensorValue &v, ElcChWriter &w) {
  w.u8(ELC_SENSOR_VALUE).u8(v.sensor).s32(v.value).u8(v.status);
}
bool elc_sensor_decode(const ElcChTransfer &t, ElcSensorDefine &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_SENSOR, ELC_SENSOR_DEFINE, r)) return false;
  v.sensor = r.u8();
  v.quantity = r.u8();
  v.exponent = r.s8();
  v.flags = r.u8();
  v.period_ms = r.u16();
  v.disp_min = r.s32();
  v.disp_max = r.s32();
  v.caution_lo = r.s32();
  v.caution_hi = r.s32();
  v.warn_lo = r.s32();
  v.warn_hi = r.s32();
  if (!r.ok()) return false;
  r.rest_text(v.name, sizeof(v.name));
  return true;
}
bool elc_sensor_decode(const ElcChTransfer &t, ElcSensorValue &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_SENSOR, ELC_SENSOR_VALUE, r)) return false;
  v.sensor = r.u8();
  v.value = r.s32();
  v.status = r.u8();
  return r.ok();
}

const char *elc_quantity_unit(uint8_t q) {
  static const char *const k[] = {"", ("\xC2\xB0" "C"), "kPa", "V", "A", "rpm", "km/h", "%", "\xCE\xBB", ":1", "m",
                                  "L", "L/h", "\xC2\xB0", "", "", "Hz", "g", "%RH", "s", "km/L"};
  return q < sizeof(k) / sizeof(k[0]) ? k[q] : "";
}

// ---- ALERT

void elc_alert_encode(const ElcAlertDefine &v, ElcChWriter &w) {
  w.u8(ELC_ALERT_DEFINE).u8(v.alert).u8(v.severity).u8(v.flags).u8(v.icon).u16(v.sound).text(v.text, 48);
}
void elc_alert_encode(const ElcAlertState &v, ElcChWriter &w) { w.u8(ELC_ALERT_STATE).u32(v.active).u8(v.generation); }
void elc_alert_encode(const ElcAlertSummary &v, ElcChWriter &w) {
  w.u8(ELC_ALERT_SUMMARY).u8(v.top_sev).u8(v.top_unacked).u8(v.count).u8(v.top_node).u8(v.top_alert);
}
void elc_alert_encode(const ElcAlertAck &v, ElcChWriter &w) { w.u8(ELC_ALERT_ACK).u8(v.dest).u8(v.alert); }
bool elc_alert_decode(const ElcChTransfer &t, ElcAlertDefine &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_ALERT, ELC_ALERT_DEFINE, r)) return false;
  v.alert = r.u8();
  v.severity = r.u8();
  v.flags = r.u8();
  v.icon = r.u8();
  v.sound = r.u16();
  if (!r.ok() || v.alert > 31) return false;
  r.rest_text(v.text, sizeof(v.text));
  return true;
}
bool elc_alert_decode(const ElcChTransfer &t, ElcAlertState &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_ALERT, ELC_ALERT_STATE, r)) return false;
  v.active = r.u32();
  v.generation = r.u8();
  return r.ok();
}
bool elc_alert_decode(const ElcChTransfer &t, ElcAlertSummary &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_ALERT, ELC_ALERT_SUMMARY, r)) return false;
  v.top_sev = r.u8();
  v.top_unacked = r.u8();
  v.count = r.u8();
  v.top_node = r.u8();
  v.top_alert = r.u8();
  return r.ok();
}
bool elc_alert_decode(const ElcChTransfer &t, ElcAlertAck &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_ALERT, ELC_ALERT_ACK, r)) return false;
  v.dest = r.u8();
  v.alert = r.u8();
  return r.ok();
}

// ---- INPUT

void elc_input_encode(const ElcInputKey &v, ElcChWriter &w) { w.u8(ELC_INPUT_KEY).u8(v.key).u8(v.gesture).u8(v.seq); }
void elc_input_encode(const ElcInputEncoder &v, ElcChWriter &w) {
  w.u8(ELC_INPUT_ENCODER).u8(v.encoder).u16((uint16_t)v.position);
}
bool elc_input_decode(const ElcChTransfer &t, ElcInputKey &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_INPUT, ELC_INPUT_KEY, r)) return false;
  v.key = r.u8();
  v.gesture = r.u8();
  v.seq = r.u8();
  return r.ok();
}
bool elc_input_decode(const ElcChTransfer &t, ElcInputEncoder &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_INPUT, ELC_INPUT_ENCODER, r)) return false;
  v.encoder = r.u8();
  v.position = (int16_t)r.u16();
  return r.ok();
}

// ---- AUDIO_CMD

void elc_audio_encode(const ElcAudioStatus &v, ElcChWriter &w) {
  w.u8(ELC_AUDIO_STATUS).u8(v.volume).u8(v.flags).u8(v.active).u8(v.stream_owner).u8(v.zones).u8(v.source).u8(v.source_node);
}
void elc_audio_encode(const ElcAudioPlay &v, ElcChWriter &w) {
  w.u8(ELC_AUDIO_PLAY).u8(v.dest).u16(v.sound).u8(v.zones_prio).u8(v.level).u8(v.repeat);
}
void elc_audio_encode(const ElcAudioStop &v, ElcChWriter &w) { w.u8(ELC_AUDIO_STOP).u8(v.dest).u16(v.sound); }
void elc_audio_encode(const ElcAudioVolume &v, ElcChWriter &w) {
  w.u8(ELC_AUDIO_VOLUME).u8(v.dest).u8(v.action).s8(v.value);
}
void elc_audio_encode(const ElcAudioMedia &v, ElcChWriter &w) { w.u8(ELC_AUDIO_MEDIA).u8(v.dest).u8(v.action).u8(v.value); }
void elc_audio_encode(const ElcAudioNow &v, ElcChWriter &w) {
  const size_t n1 = strnlen(v.line1, 32), n2 = strnlen(v.line2, 64);
  w.u8(ELC_AUDIO_NOW).u8(v.source).u8(v.flags).u8(v.preset).u8(v.signal);
  w.u8((uint8_t)n1).bytes(v.line1, n1).u8((uint8_t)n2).bytes(v.line2, n2);
}
void elc_audio_encode(const ElcAudioSource &v, ElcChWriter &w) { w.u8(ELC_AUDIO_SOURCE).u8(v.dest).u8(v.source); }
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioStatus &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_AUDIO_CMD, ELC_AUDIO_STATUS, r)) return false;
  v.volume = r.u8();
  v.flags = r.u8();
  v.active = r.u8();
  v.stream_owner = r.u8();
  v.zones = r.u8();
  if (!r.ok()) return false;
  v.source = r.left() ? r.u8() : 0;
  v.source_node = r.left() ? r.u8() : 0;
  return true;
}
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioPlay &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_AUDIO_CMD, ELC_AUDIO_PLAY, r)) return false;
  v.dest = r.u8();
  v.sound = r.u16();
  v.zones_prio = r.u8();
  v.level = r.u8();
  v.repeat = r.u8();
  return r.ok();
}
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioStop &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_AUDIO_CMD, ELC_AUDIO_STOP, r)) return false;
  v.dest = r.u8();
  v.sound = r.u16();
  return r.ok();
}
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioVolume &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_AUDIO_CMD, ELC_AUDIO_VOLUME, r)) return false;
  v.dest = r.u8();
  v.action = r.u8();
  v.value = r.s8();
  return r.ok();
}
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioMedia &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_AUDIO_CMD, ELC_AUDIO_MEDIA, r)) return false;
  v.dest = r.u8();
  v.action = r.u8();
  if (!r.ok()) return false;
  v.value = r.left() ? r.u8() : 0;
  return true;
}
const char *elc_eq_band_label(int i) {
  static const char *const kLabel[ELC_EQ_BANDS] = {"31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
  return i >= 0 && i < ELC_EQ_BANDS ? kLabel[i] : "";
}

bool elc_audio_decode(const ElcChTransfer &t, ElcAudioNow &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_AUDIO_CMD, ELC_AUDIO_NOW, r)) return false;
  v.source = r.u8();
  v.flags = r.u8();
  v.preset = r.u8();
  v.signal = r.u8();
  const uint8_t n1 = r.u8();
  for (uint8_t i = 0; i < n1; i++) {
    const uint8_t c = r.u8();
    if (i < 32) v.line1[i] = (char)c;
  }
  v.line1[n1 < 32 ? n1 : 32] = 0;
  const uint8_t n2 = r.u8();
  for (uint8_t i = 0; i < n2; i++) {
    const uint8_t c = r.u8();
    if (i < 64) v.line2[i] = (char)c;
  }
  v.line2[n2 < 64 ? n2 : 64] = 0;
  return r.ok();
}
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioSource &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_AUDIO_CMD, ELC_AUDIO_SOURCE, r)) return false;
  v.dest = r.u8();
  v.source = r.u8();
  return r.ok();
}

// ---- UI

void elc_ui_encode(const ElcUiItem &v, ElcChWriter &w) {
  w.u8(ELC_UI_ITEM).u8(v.item).u8(v.parent).u8(v.kind).u8(v.flags).s8(v.exponent).s32(v.min).s32(v.max).s32(v.step);
  w.text(v.label, 32);
  if (v.unit[0] || v.options[0]) w.u8(0).text(v.unit, 8);
  if (v.options[0]) w.u8(0).text(v.options, sizeof(v.options) - 1);
}
void elc_ui_encode(const ElcUiValue &v, ElcChWriter &w) { w.u8(ELC_UI_VALUE).u8(v.item).s32(v.value); }
void elc_ui_encode(const ElcUiText &v, ElcChWriter &w) { w.u8(ELC_UI_TEXT).u8(v.item).text(v.text, 64); }
void elc_ui_encode(const ElcUiResult &v, ElcChWriter &w) { w.u8(ELC_UI_RESULT).u8(v.item).u8(v.result).u8(v.asker); }
void elc_ui_encode(const ElcUiSet &v, ElcChWriter &w) { w.u8(ELC_UI_SET).u8(v.dest).u8(v.item).s32(v.value); }
void elc_ui_encode(const ElcUiView &v, ElcChWriter &w) { w.u8(ELC_UI_VIEW).u8(v.dest).u8(v.item); }

bool elc_ui_decode(const ElcChTransfer &t, ElcUiItem &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_UI, ELC_UI_ITEM, r)) return false;
  v.item = r.u8();
  v.parent = r.u8();
  v.kind = r.u8();
  v.flags = r.u8();
  v.exponent = r.s8();
  v.min = r.s32();
  v.max = r.s32();
  v.step = r.s32();
  if (!r.ok()) return false;
  r.field(v.label, sizeof(v.label));
  r.field(v.unit, sizeof(v.unit));
  r.rest_text(v.options, sizeof(v.options));
  return true;
}
bool elc_ui_decode(const ElcChTransfer &t, ElcUiValue &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_UI, ELC_UI_VALUE, r)) return false;
  v.item = r.u8();
  v.value = r.s32();
  return r.ok();
}
bool elc_ui_decode(const ElcChTransfer &t, ElcUiText &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_UI, ELC_UI_TEXT, r)) return false;
  v.item = r.u8();
  if (!r.ok()) return false;
  r.rest_text(v.text, sizeof(v.text));
  return true;
}
bool elc_ui_decode(const ElcChTransfer &t, ElcUiResult &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_UI, ELC_UI_RESULT, r)) return false;
  v.item = r.u8();
  v.result = r.u8();
  v.asker = r.u8();
  return r.ok();
}
bool elc_ui_decode(const ElcChTransfer &t, ElcUiSet &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_UI, ELC_UI_SET, r)) return false;
  v.dest = r.u8();
  v.item = r.u8();
  v.value = r.s32();
  return r.ok();
}
bool elc_ui_decode(const ElcChTransfer &t, ElcUiView &v) {
  ElcChReader r(nullptr, 0);
  if (!open(t, ELC_CH_UI, ELC_UI_VIEW, r)) return false;
  v.dest = r.u8();
  v.item = r.u8();
  return r.ok();
}

bool ElcUiItem::option(int k, char *out, size_t cap) const {
  const char *p = options;
  for (int i = 0; i < k; i++) {
    p = strchr(p, '\n');
    if (!p) return false;
    p++;
  }
  if (!*p && k > 0) return false;
  size_t n = 0;
  while (p[n] && p[n] != '\n') n++;
  if (n + 1 > cap) n = cap - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

int ElcUiItem::option_count() const {
  if (!options[0]) return 0;
  int n = 1;
  for (const char *p = options; *p; p++) n += *p == '\n';
  return n;
}

void elc_ui_format(int32_t value, int8_t exponent, char *out, size_t cap) {
  if (exponent >= 0) {
    long long v = value;
    for (int i = 0; i < exponent; i++) v *= 10;
    snprintf(out, cap, "%lld", v);
    return;
  }
  long long div = 1;
  for (int i = 0; i < -exponent; i++) div *= 10;
  const long long a = value < 0 ? -(long long)value : value;
  snprintf(out, cap, "%s%lld.%0*lld", value < 0 ? "-" : "", a / div, -exponent, a % div);
}

// (copy() kept for callers that fill fixed text fields)
void elc_ch_copy_text(char *dst, size_t cap, const char *src) { copy(dst, cap, src); }
