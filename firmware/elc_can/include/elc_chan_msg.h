#pragma once

// Messages on the channels (docs/protocols/can_channels.md): SYSTEM, LOG, SENSOR, ALERT, INPUT, AUDIO_CMD, UI.
// Each encode_* builds a transfer payload (send it with ElcChannels::send(base, writer)); each decode_* reads one from
// a received transfer and returns false when the opcode or length is wrong. Trailing bytes a later version adds are
// ignored. No Arduino dependencies: host-tested.

#include <stddef.h>
#include <stdint.h>

#include "elc_chan.h"

static const int32_t ELC_CH_NONE32 = (int32_t)0x80000000;

// ---- SYSTEM (0x060 + node), from the center ----
static const uint8_t ELC_SYS_TIME = 0x01, ELC_SYS_VEHICLE = 0x02, ELC_SYS_PREFS = 0x03, ELC_SYS_POWER = 0x04;
static const uint16_t ELC_VEH_IGNITION = 1 << 0, ELC_VEH_RUNNING = 1 << 1, ELC_VEH_HEADLIGHTS = 1 << 2,
                      ELC_VEH_REVERSE = 1 << 3, ELC_VEH_MOVING = 1 << 4, ELC_VEH_SENSORS_LIVE = 1 << 5,
                      ELC_VEH_NIGHT = 1 << 6;
static const uint8_t ELC_PREFS_METRIC = 1 << 0, ELC_PREFS_CELSIUS = 1 << 1, ELC_PREFS_KPA = 1 << 2;
static const uint8_t ELC_TIME_SRC_NONE = 0, ELC_TIME_SRC_GPS = 1, ELC_TIME_SRC_RTC = 3;
static const uint8_t ELC_POWER_IGN_OFF = 1, ELC_POWER_IGN_ON = 2;

struct ElcSysTime { uint32_t utc = 0; int8_t offset_q = 0; uint8_t source = 0; };  // offset in 15-minute steps
struct ElcSysVehicle { uint16_t flags = 0; uint8_t backlight = 100; uint16_t speed_x10 = 0xFFFF; };  // km/h x 10
struct ElcSysPrefs { uint8_t units = 0; uint8_t theme = 0; uint8_t clock24 = 0; };
struct ElcSysPower { uint8_t event = 0; uint8_t seconds = 0; };
void elc_sys_encode(const ElcSysTime &v, ElcChWriter &w);
void elc_sys_encode(const ElcSysVehicle &v, ElcChWriter &w);
void elc_sys_encode(const ElcSysPrefs &v, ElcChWriter &w);
void elc_sys_encode(const ElcSysPower &v, ElcChWriter &w);
bool elc_sys_decode(const ElcChTransfer &t, ElcSysTime &v);
bool elc_sys_decode(const ElcChTransfer &t, ElcSysVehicle &v);
bool elc_sys_decode(const ElcChTransfer &t, ElcSysPrefs &v);
bool elc_sys_decode(const ElcChTransfer &t, ElcSysPower &v);

// ---- LOG (0x3E0 + node) ----
static const uint8_t ELC_LOG_LINE = 0x01;
static const uint8_t ELC_LOG_DEBUG = 0, ELC_LOG_INFO = 1, ELC_LOG_WARN = 2, ELC_LOG_ERROR = 3;
struct ElcLogLine { uint8_t level = ELC_LOG_INFO; char text[121] = {0}; };
void elc_log_encode(const ElcLogLine &v, ElcChWriter &w);
bool elc_log_decode(const ElcChTransfer &t, ElcLogLine &v);

// ---- SENSOR (0x120 + node) ----
static const uint8_t ELC_SENSOR_DEFINE = 0x01, ELC_SENSOR_VALUE = 0x02;
// quantities (Registries)
static const uint8_t ELC_Q_UNITLESS = 0, ELC_Q_TEMPERATURE = 1, ELC_Q_PRESSURE = 2, ELC_Q_VOLTAGE = 3, ELC_Q_CURRENT = 4,
                     ELC_Q_RPM = 5, ELC_Q_SPEED = 6, ELC_Q_PERCENT = 7, ELC_Q_LAMBDA = 8, ELC_Q_AFR = 9,
                     ELC_Q_DISTANCE = 10, ELC_Q_VOLUME = 11, ELC_Q_FLOW = 12, ELC_Q_ANGLE = 13, ELC_Q_SWITCH = 14,
                     ELC_Q_COUNT = 15, ELC_Q_FREQUENCY = 16, ELC_Q_ACCEL = 17, ELC_Q_HUMIDITY = 18,
                     ELC_Q_DURATION = 19, ELC_Q_ECONOMY = 20;
static const uint8_t ELC_SENSOR_F_LOG = 1, ELC_SENSOR_F_GAUGE = 2, ELC_SENSOR_F_ALERTS = 4;
static const uint8_t ELC_SENSOR_OK = 0, ELC_SENSOR_NOT_READY = 1, ELC_SENSOR_OPEN = 2, ELC_SENSOR_SHORT = 3,
                     ELC_SENSOR_RANGE = 4, ELC_SENSOR_FAULT = 5;
struct ElcSensorDefine {
  uint8_t sensor = 0, quantity = 0;
  int8_t exponent = 0;
  uint8_t flags = 0;
  uint16_t period_ms = 1000;
  int32_t disp_min = 0, disp_max = 0;
  int32_t caution_lo = ELC_CH_NONE32, caution_hi = ELC_CH_NONE32, warn_lo = ELC_CH_NONE32, warn_hi = ELC_CH_NONE32;
  char name[21] = {0};
};
struct ElcSensorValue { uint8_t sensor = 0; int32_t value = 0; uint8_t status = 0; };
void elc_sensor_encode(const ElcSensorDefine &v, ElcChWriter &w);
void elc_sensor_encode(const ElcSensorValue &v, ElcChWriter &w);
bool elc_sensor_decode(const ElcChTransfer &t, ElcSensorDefine &v);
bool elc_sensor_decode(const ElcChTransfer &t, ElcSensorValue &v);
/** Unit text for a quantity in its canonical unit ("°C", "kPa", ...). */
const char *elc_quantity_unit(uint8_t q);

// ---- ALERT (0x020 + node) ----
static const uint8_t ELC_ALERT_DEFINE = 0x01, ELC_ALERT_STATE = 0x02, ELC_ALERT_SUMMARY = 0x03, ELC_ALERT_ACK = 0x81;
static const uint8_t ELC_SEV_INFO = 0, ELC_SEV_ADVISORY = 1, ELC_SEV_CAUTION = 2, ELC_SEV_WARNING = 3;
static const uint8_t ELC_ALERT_F_ACK = 1, ELC_ALERT_F_LATCH = 2, ELC_ALERT_F_SILENT = 4;
struct ElcAlertDefine {
  uint8_t alert = 0, severity = 0, flags = 0, icon = 0;
  uint16_t sound = 0;
  char text[49] = {0};
};
struct ElcAlertState { uint32_t active = 0; uint8_t generation = 0; };
struct ElcAlertSummary { uint8_t top_sev = 0xFF, top_unacked = 0xFF, count = 0, top_node = 0, top_alert = 0; };
struct ElcAlertAck { uint8_t dest = 0, alert = 0; };
void elc_alert_encode(const ElcAlertDefine &v, ElcChWriter &w);
void elc_alert_encode(const ElcAlertState &v, ElcChWriter &w);
void elc_alert_encode(const ElcAlertSummary &v, ElcChWriter &w);
void elc_alert_encode(const ElcAlertAck &v, ElcChWriter &w);
bool elc_alert_decode(const ElcChTransfer &t, ElcAlertDefine &v);
bool elc_alert_decode(const ElcChTransfer &t, ElcAlertState &v);
bool elc_alert_decode(const ElcChTransfer &t, ElcAlertSummary &v);
bool elc_alert_decode(const ElcChTransfer &t, ElcAlertAck &v);

// ---- INPUT (0x040 + node) ----
static const uint8_t ELC_INPUT_KEY = 0x01, ELC_INPUT_ENCODER = 0x02;
static const uint8_t ELC_KEY_UP = 1, ELC_KEY_DOWN = 2, ELC_KEY_LEFT = 3, ELC_KEY_RIGHT = 4, ELC_KEY_SELECT = 5,
                     ELC_KEY_BACK = 6, ELC_KEY_VIEW = 7, ELC_KEY_HOME = 8, ELC_KEY_PLAY = 16, ELC_KEY_NEXT = 17,
                     ELC_KEY_PREV = 18, ELC_KEY_VOL_UP = 19, ELC_KEY_VOL_DOWN = 20, ELC_KEY_MUTE = 21, ELC_KEY_F1 = 32;
static const uint8_t ELC_GEST_CLICK = 1, ELC_GEST_LONG = 2, ELC_GEST_REPEAT = 3, ELC_GEST_DOWN = 4, ELC_GEST_UP = 5;
static const uint8_t ELC_ENC_NAV = 0, ELC_ENC_VOLUME = 1;
struct ElcInputKey { uint8_t key = 0, gesture = 0, seq = 0; };
struct ElcInputEncoder { uint8_t encoder = 0; int16_t position = 0; };
void elc_input_encode(const ElcInputKey &v, ElcChWriter &w);
void elc_input_encode(const ElcInputEncoder &v, ElcChWriter &w);
bool elc_input_decode(const ElcChTransfer &t, ElcInputKey &v);
bool elc_input_decode(const ElcChTransfer &t, ElcInputEncoder &v);

// ---- AUDIO_CMD (0x240 + node) ----
static const uint8_t ELC_AUDIO_STATUS = 0x01, ELC_AUDIO_DONE = 0x02, ELC_AUDIO_GRANT = 0x03, ELC_AUDIO_NOW = 0x04,
                     ELC_AUDIO_PLAY = 0x81, ELC_AUDIO_STOP = 0x82, ELC_AUDIO_VOLUME = 0x83, ELC_AUDIO_MEDIA = 0x84,
                     ELC_AUDIO_SOURCE = 0x87;
static const uint8_t ELC_AUDIO_F_MUTED = 1, ELC_AUDIO_F_PLAYING = 2, ELC_AUDIO_F_DUCKED = 4;
static const uint8_t ELC_MEDIA_TOGGLE = 1, ELC_MEDIA_PLAY = 2, ELC_MEDIA_PAUSE = 3, ELC_MEDIA_NEXT = 4, ELC_MEDIA_PREV = 5,
                     ELC_MEDIA_TUNE_UP = 6, ELC_MEDIA_TUNE_DOWN = 7, ELC_MEDIA_PRESET_NEXT = 8, ELC_MEDIA_PRESET_PREV = 9,
                     ELC_MEDIA_PRESET_STORE = 10, ELC_MEDIA_SEEK_UP = 11, ELC_MEDIA_SEEK_DOWN = 12;
// sources (AUDIO_CMD, Sources)
static const uint8_t ELC_SRC_NONE = 0, ELC_SRC_BLUETOOTH = 1, ELC_SRC_RADIO = 2, ELC_SRC_AUX = 3;
static const uint8_t ELC_NOW_F_STEREO = 1, ELC_NOW_F_RDS = 2, ELC_NOW_F_MUTED = 4, ELC_NOW_F_SEEKING = 8;
/** source / source_node read as 0 from a sink that does not send them (they were added at the end). */
struct ElcAudioStatus { uint8_t volume = 0, flags = 0, active = 0, stream_owner = 0, zones = 0, source = 0, source_node = 0; };
struct ElcAudioNow {
  uint8_t source = 0, flags = 0, preset = 0, signal = 0xFF;
  char line1[33] = {0};
  char line2[65] = {0};
};
struct ElcAudioSource { uint8_t dest = 0, source = 0; };
struct ElcAudioPlay { uint8_t dest = 0; uint16_t sound = 0; uint8_t zones_prio = 0, level = 100, repeat = 1; };
struct ElcAudioStop { uint8_t dest = 0; uint16_t sound = 0xFFFF; };
struct ElcAudioVolume { uint8_t dest = 0, action = 0; int8_t value = 0; };
struct ElcAudioMedia { uint8_t dest = 0, action = 0, value = 0; };
void elc_audio_encode(const ElcAudioStatus &v, ElcChWriter &w);
void elc_audio_encode(const ElcAudioPlay &v, ElcChWriter &w);
void elc_audio_encode(const ElcAudioStop &v, ElcChWriter &w);
void elc_audio_encode(const ElcAudioVolume &v, ElcChWriter &w);
void elc_audio_encode(const ElcAudioMedia &v, ElcChWriter &w);
void elc_audio_encode(const ElcAudioNow &v, ElcChWriter &w);
void elc_audio_encode(const ElcAudioSource &v, ElcChWriter &w);
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioNow &v);
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioSource &v);
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioStatus &v);
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioPlay &v);
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioStop &v);
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioVolume &v);
bool elc_audio_decode(const ElcChTransfer &t, ElcAudioMedia &v);

// ---- UI (0x260 + node) ----
static const uint8_t ELC_UI_ITEM = 0x01, ELC_UI_VALUE = 0x02, ELC_UI_TEXT = 0x03, ELC_UI_RESULT = 0x04,
                     ELC_UI_SET = 0x81, ELC_UI_VIEW = 0x82;
static const uint8_t ELC_UI_GROUP = 0, ELC_UI_READOUT = 1, ELC_UI_TOGGLE = 2, ELC_UI_NUMBER = 3, ELC_UI_CHOICE = 4,
                     ELC_UI_ACTION = 5, ELC_UI_TEXTOUT = 6;
static const uint8_t ELC_UI_F_RO = 1, ELC_UI_F_CONFIRM = 2, ELC_UI_F_ADVANCED = 4, ELC_UI_F_LIVE = 8,
                     ELC_UI_F_SETTING = 16;
static const uint8_t ELC_UI_TOP = 0xFF;
/** VIEW's item when the accessory's top page shows (0xFF there means the page closed). */
static const uint8_t ELC_UI_VIEW_TOP = 0xFE;
static const uint8_t ELC_UI_VIEW_CLOSED = 0xFF;
static const uint8_t ELC_UI_OK = 0, ELC_UI_RANGE = 1, ELC_UI_READONLY = 2, ELC_UI_BUSY = 3, ELC_UI_UNKNOWN = 4;
struct ElcUiItem {
  uint8_t item = 0, parent = ELC_UI_TOP, kind = 0, flags = 0;
  int8_t exponent = 0;
  int32_t min = 0, max = 0, step = 0;
  char label[33] = {0};
  char unit[9] = {0};
  char options[160] = {0};  // '\n'-separated
  /** Option k (0-based) of a choice into out; false past the end. */
  bool option(int k, char *out, size_t cap) const;
  int option_count() const;
};
struct ElcUiValue { uint8_t item = 0; int32_t value = 0; };
struct ElcUiText { uint8_t item = 0; char text[65] = {0}; };
struct ElcUiResult { uint8_t item = 0, result = 0, asker = 0; };
struct ElcUiSet { uint8_t dest = 0, item = 0; int32_t value = 0; };
struct ElcUiView { uint8_t dest = 0, item = ELC_UI_TOP; };
void elc_ui_encode(const ElcUiItem &v, ElcChWriter &w);
void elc_ui_encode(const ElcUiValue &v, ElcChWriter &w);
void elc_ui_encode(const ElcUiText &v, ElcChWriter &w);
void elc_ui_encode(const ElcUiResult &v, ElcChWriter &w);
void elc_ui_encode(const ElcUiSet &v, ElcChWriter &w);
void elc_ui_encode(const ElcUiView &v, ElcChWriter &w);
bool elc_ui_decode(const ElcChTransfer &t, ElcUiItem &v);
bool elc_ui_decode(const ElcChTransfer &t, ElcUiValue &v);
bool elc_ui_decode(const ElcChTransfer &t, ElcUiText &v);
bool elc_ui_decode(const ElcChTransfer &t, ElcUiResult &v);
bool elc_ui_decode(const ElcChTransfer &t, ElcUiSet &v);
bool elc_ui_decode(const ElcChTransfer &t, ElcUiView &v);
/** Copies text into a fixed field, cut to cap - 1, NUL-terminated. */
void elc_ch_copy_text(char *dst, size_t cap, const char *src);
/** "94.7" from value 947, exponent -1. */
void elc_ui_format(int32_t value, int8_t exponent, char *out, size_t cap);
