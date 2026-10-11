#include "phone.h"

#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <ctype.h>
#include <string.h>

#include "BluetoothA2DPSink.h"
#include "audio_engine.h"
#include "esp_hf_client_api.h"
#include "esp_hf_client_legacy_api.h"

namespace {

constexpr uint32_t kPairWindowMs = 120000;
constexpr uint32_t kConfirmTimeoutMs = 30000;
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint8_t kDefaultCallVolume = 12;

BluetoothA2DPSink *s_sink = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> s_changes{0};

struct Paired {
  esp_bd_addr_t bda;
  char name[kPhoneTextMax + 1];
};
// Paired list: loop task only, except reads of names under s_mux for phone_get (the callbacks never touch it).
Paired s_list[kPhoneMaxPaired];
uint8_t s_count = 0;
uint8_t s_list_serial = 0;
bool s_list_dirty = false;

// ---- everything below is shared with the Bluetooth callbacks: under s_mux
bool s_a2dp = false;
esp_bd_addr_t s_a2dp_bda = {0};
uint32_t s_a2dp_since_ms = 0;
esp_hf_client_connection_state_t s_hfp_state = ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED;
bool s_slc = false;  // service level connection: the hands-free link is usable
esp_bd_addr_t s_hfp_bda = {0};
uint32_t s_hfp_since_ms = 0;

int s_ind_call = 0, s_ind_setup = 0, s_ind_held = 0;
uint8_t s_call = kCallIdle;
uint32_t s_call_start_ms = 0;
char s_caller[kPhoneTextMax + 1] = {0};
uint8_t s_caller_serial = 0;
bool s_sco = false, s_mic_muted = false, s_inband = false, s_voice = false;
uint8_t s_call_vol = kDefaultCallVolume;
uint8_t s_signal = kPhoneNone, s_battery = kPhoneNone;
bool s_want_clcc = false;

uint32_t s_pair_until_ms = 0;  // pairing window on command (0 = closed)
bool s_pair_auto = false;       // open with no time limit: nothing paired (at boot, or after the last Forget)
bool s_confirm = false, s_confirm_reply = false;  // a code is shown; reply = the stack waits for our yes / no
esp_bd_addr_t s_confirm_bda = {0};
uint32_t s_passkey = 0, s_confirm_until_ms = 0;
char s_pairing_name[kPhoneTextMax + 1] = {0};
uint8_t s_pairing_serial = 0;

// Handed from the callbacks to the loop (NVS and list changes happen there).
bool s_new_bond = false;
esp_bd_addr_t s_new_bond_bda;
char s_new_bond_name[kPhoneTextMax + 1];
bool s_touch = false;  // a phone connected: move it to the top of the list
esp_bd_addr_t s_touch_bda;
bool s_named = false;  // a remote name arrived
esp_bd_addr_t s_named_bda;
char s_named_name[kPhoneTextMax + 1];

// Loop task only.
bool s_scan_open = false, s_scan_applied = false;
bool s_target = false;  // connect to a chosen phone
esp_bd_addr_t s_target_bda;
uint32_t s_target_ms = 0;
bool s_target_dropped = false, s_target_dialed = false;
uint32_t s_hfp_try_ms = 0;
uint8_t s_hfp_tries = 0;
bool s_a2dp_tried = false;
uint32_t s_cod_at_ms = 0;
Preferences s_prefs;

void changed() { s_changes++; }

bool same(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, ESP_BD_ADDR_LEN) == 0; }

/** Copies a name or text; curly quotes become straight ones ("Rob’s iPhone"): the screens' fonts have no curly ones. */
void copy_text(char *dst, const char *src) {
  size_t n = 0;
  for (const char *p = src ? src : ""; *p && n < kPhoneTextMax; p++) {
    const uint8_t *u = reinterpret_cast<const uint8_t *>(p);
    if (u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0x98 || u[2] == 0x99 || u[2] == 0x9C || u[2] == 0x9D)) {
      dst[n++] = u[2] >= 0x9C ? '"' : '\'';
      p += 2;
    } else {
      dst[n++] = *p;
    }
  }
  dst[n] = '\0';
}

/** "+15550104477" -> "(555) 010-4477" for North American numbers; anything else as the phone sent it. */
void format_number(const char *in, char *out) {
  char d[24];
  size_t n = 0;
  for (const char *p = in; *p && n < sizeof(d) - 1; p++) {
    if (isdigit((unsigned char)*p)) d[n++] = *p;
  }
  d[n] = '\0';
  const char *x = d;
  if (n == 11 && d[0] == '1') {
    x++;
    n--;
  }
  if (n == 10 && (!in[0] || in[0] == '+' || isdigit((unsigned char)in[0]) || in[0] == '(')) {
    snprintf(out, kPhoneTextMax + 1, "(%.3s) %.3s-%.4s", x, x + 3, x + 6);
  } else if (in[0]) {
    copy_text(out, in);
  } else {
    copy_text(out, "Unknown");
  }
}

/** Under s_mux. */
void set_caller_locked(const char *number) {
  char f[kPhoneTextMax + 1];
  format_number(number ? number : "", f);
  if (strcmp(f, s_caller) != 0) {
    memcpy(s_caller, f, sizeof(s_caller));
    s_caller_serial = (uint8_t)((s_caller_serial + 1) & 0x0F);
  }
}

/** The call state from the phone's three indicators. Under s_mux. */
void derive_call_locked() {
  uint8_t st = kCallIdle;
  if (s_slc) {
    if (s_ind_call == ESP_HF_CALL_STATUS_NO_CALLS) {
      if (s_ind_setup == ESP_HF_CALL_SETUP_STATUS_INCOMING) st = kCallIncoming;
      else if (s_ind_setup == ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING) st = kCallDialing;
      else if (s_ind_setup == ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING) st = kCallAlerting;
      else if (s_ind_held != ESP_HF_CALL_HELD_STATUS_NONE) st = kCallHeld;
    } else if (s_ind_setup == ESP_HF_CALL_SETUP_STATUS_INCOMING) {
      st = kCallWaiting;
    } else if (s_ind_setup == ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING) {
      st = kCallDialing;
    } else if (s_ind_setup == ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING) {
      st = kCallAlerting;
    } else if (s_ind_held != ESP_HF_CALL_HELD_STATUS_NONE) {
      st = kCallHeld;
    } else {
      st = kCallActive;
    }
  }
  if (s_ind_call != ESP_HF_CALL_STATUS_NO_CALLS && s_call_start_ms == 0) s_call_start_ms = millis();
  if (s_ind_call == ESP_HF_CALL_STATUS_NO_CALLS) s_call_start_ms = 0;
  if (st == s_call) return;
  s_call = st;
  if (st == kCallIdle) {
    if (s_caller[0]) {
      s_caller[0] = '\0';
      s_caller_serial = (uint8_t)((s_caller_serial + 1) & 0x0F);
    }
    s_mic_muted = false;  // the next call starts unmuted
    call_audio_set_mute(false);
  } else {
    s_want_clcc = true;  // ask the phone which number goes with the new state
  }
  changed();
}

/** Which +CLCC status names the call the current state is about. */
int clcc_wanted_locked() {
  switch (s_call) {
    case kCallIncoming: return ESP_HF_CURRENT_CALL_STATUS_INCOMING;
    case kCallWaiting: return ESP_HF_CURRENT_CALL_STATUS_WAITING;
    case kCallDialing: return ESP_HF_CURRENT_CALL_STATUS_DIALING;
    case kCallAlerting: return ESP_HF_CURRENT_CALL_STATUS_ALERTING;
    case kCallActive: return ESP_HF_CURRENT_CALL_STATUS_ACTIVE;
    case kCallHeld:
      return s_ind_held == ESP_HF_CALL_HELD_STATUS_HELD ? ESP_HF_CURRENT_CALL_STATUS_HELD
                                                         : ESP_HF_CURRENT_CALL_STATUS_ACTIVE;
    default: return -1;
  }
}

/** Phone -> car PCM (the stack decoded it). Each frame in lets one frame of microphone go out. */
void hf_audio_in(const uint8_t *buf, uint32_t len) {
  call_audio_rx(buf, len, false);
  esp_hf_client_outgoing_data_ready();
}

uint32_t hf_audio_out(uint8_t *buf, uint32_t len) { return call_audio_tx(buf, len); }

void hf_event(esp_hf_client_cb_event_t ev, esp_hf_client_cb_param_t *p) {
  switch (ev) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT: {
      const esp_hf_client_connection_state_t st = p->conn_stat.state;
      Serial.printf("hfp: connection state %d (heap %u free, %u min)\n", (int)st, (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getMinFreeHeap());
      bool drop_audio = false;
      portENTER_CRITICAL(&s_mux);
      s_hfp_state = st;
      memcpy(s_hfp_bda, p->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
      if (st == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
        s_slc = true;
        s_hfp_since_ms = millis();
        s_inband = (p->conn_stat.peer_feat & ESP_HF_CLIENT_PEER_FEAT_INBAND) != 0;
        s_touch = true;
        memcpy(s_touch_bda, p->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
      } else if (st == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
        s_slc = false;
        drop_audio = s_sco;
        s_sco = false;
        s_ind_call = s_ind_setup = s_ind_held = 0;
        s_signal = s_battery = kPhoneNone;
        s_voice = false;
        derive_call_locked();
      }
      changed();
      portEXIT_CRITICAL(&s_mux);
      if (drop_audio) call_audio_stop();
      break;
    }
    case ESP_HF_CLIENT_AUDIO_STATE_EVT: {
      const esp_hf_client_audio_state_t st = p->audio_stat.state;
      const bool up = st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED || st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC;
      const bool down = st == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED;
      if (up) {
        const uint32_t rate = st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC ? 16000 : 8000;
        Serial.printf("hfp: call audio up, %s, frame %u bytes\n", rate == 16000 ? "mSBC 16 kHz" : "CVSD 8 kHz",
                      (unsigned)p->audio_stat.preferred_frame_size);
        call_audio_start(rate);
      } else if (down) {
        Serial.println("hfp: call audio down");
        call_audio_stop();
      }
      if (up || down) {
        portENTER_CRITICAL(&s_mux);
        s_sco = up;
        changed();
        portEXIT_CRITICAL(&s_mux);
      }
      break;
    }
    case ESP_HF_CLIENT_BVRA_EVT:
      portENTER_CRITICAL(&s_mux);
      s_voice = p->bvra.value == ESP_HF_VR_STATE_ENABLED;
      changed();
      portEXIT_CRITICAL(&s_mux);
      break;
    case ESP_HF_CLIENT_CIND_CALL_EVT:
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
    case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
      portENTER_CRITICAL(&s_mux);
      if (ev == ESP_HF_CLIENT_CIND_CALL_EVT) s_ind_call = p->call.status;
      if (ev == ESP_HF_CLIENT_CIND_CALL_SETUP_EVT) s_ind_setup = p->call_setup.status;
      if (ev == ESP_HF_CLIENT_CIND_CALL_HELD_EVT) s_ind_held = p->call_held.status;
      derive_call_locked();
      portEXIT_CRITICAL(&s_mux);
      break;
    case ESP_HF_CLIENT_CIND_SIGNAL_STRENGTH_EVT:
    case ESP_HF_CLIENT_CIND_BATTERY_LEVEL_EVT:
      portENTER_CRITICAL(&s_mux);
      if (ev == ESP_HF_CLIENT_CIND_SIGNAL_STRENGTH_EVT) s_signal = (uint8_t)p->signal_strength.value;
      else s_battery = (uint8_t)p->battery_level.value;
      changed();
      portEXIT_CRITICAL(&s_mux);
      break;
    case ESP_HF_CLIENT_CLIP_EVT:
    case ESP_HF_CLIENT_CCWA_EVT: {
      const char *num = ev == ESP_HF_CLIENT_CLIP_EVT ? p->clip.number : p->ccwa.number;
      portENTER_CRITICAL(&s_mux);
      if ((ev == ESP_HF_CLIENT_CLIP_EVT && s_call == kCallIncoming) || (ev == ESP_HF_CLIENT_CCWA_EVT && s_call == kCallWaiting)) {
        set_caller_locked(num);
        changed();
      }
      portEXIT_CRITICAL(&s_mux);
      break;
    }
    case ESP_HF_CLIENT_CLCC_EVT:
      portENTER_CRITICAL(&s_mux);
      if ((int)p->clcc.status == clcc_wanted_locked() && p->clcc.number && p->clcc.number[0]) {
        set_caller_locked(p->clcc.number);
        changed();
      }
      portEXIT_CRITICAL(&s_mux);
      break;
    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
      if (p->volume_control.type == ESP_HF_VOLUME_CONTROL_TARGET_SPK) {
        const int v = p->volume_control.volume < 0 ? 0 : (p->volume_control.volume > 15 ? 15 : p->volume_control.volume);
        call_audio_set_volume((uint8_t)v);
        portENTER_CRITICAL(&s_mux);
        s_call_vol = (uint8_t)v;
        changed();
        portEXIT_CRITICAL(&s_mux);
      }
      break;
    case ESP_HF_CLIENT_BSIR_EVT:
      portENTER_CRITICAL(&s_mux);
      s_inband = p->bsir.state == ESP_HF_CLIENT_IN_BAND_RINGTONE_PROVIDED;
      changed();
      portEXIT_CRITICAL(&s_mux);
      break;
    default:
      break;
  }
}

// ---- paired list (loop task) ----

int list_find(const uint8_t *bda) {
  for (uint8_t i = 0; i < s_count; i++) {
    if (same(s_list[i].bda, bda)) return i;
  }
  return -1;
}

void list_changed() {
  s_list_serial = (uint8_t)((s_list_serial + 1) & 0x0F);
  s_list_dirty = true;
  changed();
}

void default_name(const uint8_t *bda, char *out) {
  snprintf(out, kPhoneTextMax + 1, "Phone %02X%02X", bda[4], bda[5]);
}

/** Put a phone at the top of the list (adding it if new). Returns false when nothing changed. */
bool list_to_front(const uint8_t *bda, const char *name) {
  int i = list_find(bda);
  Paired p;
  if (i >= 0) {
    p = s_list[i];
    if (i == 0 && !(name && name[0] && strcmp(name, p.name) != 0)) return false;
  } else {
    memcpy(p.bda, bda, ESP_BD_ADDR_LEN);
    default_name(bda, p.name);
    if (s_count == kPhoneMaxPaired) {
      // Full: the least recently used phone makes room (and loses its bond).
      Serial.printf("phones: forgetting %s to make room\n", s_list[s_count - 1].name);
      esp_bt_gap_remove_bond_device(s_list[s_count - 1].bda);
      s_count--;
    }
    i = s_count++;
  }
  if (name && name[0]) copy_text(p.name, name);
  portENTER_CRITICAL(&s_mux);
  memmove(&s_list[1], &s_list[0], sizeof(Paired) * (size_t)i);
  s_list[0] = p;
  portEXIT_CRITICAL(&s_mux);
  list_changed();
  return true;
}

void list_remove(int i) {
  portENTER_CRITICAL(&s_mux);
  memmove(&s_list[i], &s_list[i + 1], sizeof(Paired) * (size_t)(s_count - i - 1));
  s_count--;
  portEXIT_CRITICAL(&s_mux);
  list_changed();
}

void list_save() {
  uint8_t blob[1 + kPhoneMaxPaired * sizeof(Paired)];
  blob[0] = s_count;
  memcpy(blob + 1, s_list, s_count * sizeof(Paired));
  s_prefs.putBytes("list", blob, 1 + s_count * sizeof(Paired));
  s_list_dirty = false;
}

/** Our list, trimmed to what Bluedroid still has bonded, plus bonds we did not know (named when they connect). */
void list_load() {
  uint8_t blob[1 + kPhoneMaxPaired * sizeof(Paired)];
  const size_t n = s_prefs.getBytes("list", blob, sizeof(blob));
  s_count = 0;
  if (n >= 1 && blob[0] <= kPhoneMaxPaired && n >= 1 + blob[0] * sizeof(Paired)) {
    s_count = blob[0];
    memcpy(s_list, blob + 1, s_count * sizeof(Paired));
  }
  esp_bd_addr_t bonded[16];
  int nb = esp_bt_gap_get_bond_device_num();
  if (nb > 16) nb = 16;
  if (nb < 0 || esp_bt_gap_get_bond_device_list(&nb, bonded) != ESP_OK) nb = 0;
  for (int i = (int)s_count - 1; i >= 0; i--) {
    bool ok = false;
    for (int b = 0; b < nb; b++) ok |= same(s_list[i].bda, bonded[b]);
    if (!ok) {
      memmove(&s_list[i], &s_list[i + 1], sizeof(Paired) * (size_t)(s_count - i - 1));
      s_count--;
    }
  }
  for (int b = 0; b < nb && s_count < kPhoneMaxPaired; b++) {
    if (list_find(bonded[b]) < 0) {
      memcpy(s_list[s_count].bda, bonded[b], ESP_BD_ADDR_LEN);
      default_name(bonded[b], s_list[s_count].name);
      s_count++;
    }
  }
  list_save();
  Serial.printf("phones: %u paired (%d bonds)\n", (unsigned)s_count, nb);
  for (uint8_t i = 0; i < s_count; i++) Serial.printf("  %u: %s\n", (unsigned)i, s_list[i].name);
}

/** The phone we are connected to (HFP first, else A2DP). Under s_mux. */
bool connected_bda_locked(esp_bd_addr_t out) {
  if (s_slc) {
    memcpy(out, s_hfp_bda, ESP_BD_ADDR_LEN);
    return true;
  }
  if (s_a2dp) {
    memcpy(out, s_a2dp_bda, ESP_BD_ADDR_LEN);
    return true;
  }
  return false;
}

bool connected_bda(esp_bd_addr_t out) {
  portENTER_CRITICAL(&s_mux);
  const bool c = connected_bda_locked(out);
  portEXIT_CRITICAL(&s_mux);
  return c;
}

void disconnect_all() {
  esp_bd_addr_t b;
  bool slc;
  portENTER_CRITICAL(&s_mux);
  slc = s_slc || s_hfp_state != ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED;
  memcpy(b, s_hfp_bda, ESP_BD_ADDR_LEN);
  portEXIT_CRITICAL(&s_mux);
  if (slc) esp_hf_client_disconnect(b);
  if (s_sink && s_sink->is_connected()) s_sink->disconnect();
}

bool pairing_window_open() { return s_pair_auto || s_pair_until_ms != 0; }

void apply_scan_mode() {
  const bool open = pairing_window_open();
  if (s_scan_applied && open == s_scan_open) return;
  s_scan_open = open;
  s_scan_applied = true;
  // Always connectable (paired phones come back on their own); discoverable only while pairing.
  s_sink->set_discoverability(open ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, open ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
  Serial.printf("phones: pairing window %s\n", open ? "open (discoverable)" : "closed");
  changed();
}

void set_class_of_device() {
  // A car kit: phones then offer it for calls as well as media.
  esp_bt_cod_t cod = {};
  cod.major = ESP_BT_COD_MAJOR_DEV_AV;
  cod.minor = 0x08;  // audio/video: car audio
  cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_TELEPHONY;
  esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
}

}  // namespace

void phone_begin(BluetoothA2DPSink *sink) {
  s_sink = sink;
  // IO capability "no input, no output": Just Works pairing. With "display yes/no" an iPhone pairs Just Works anyway,
  // then the profiles ask for an authenticated key, the stack starts a second pairing to upgrade it, and iOS never
  // shows that request (it times out, status 9). The pairing window is what decides who may pair.
  sink->activate_pin_code(false);
  sink->set_discoverability(ESP_BT_NON_DISCOVERABLE);  // phone_loop opens the window when there is reason to
  call_audio_set_volume(s_call_vol);
}

void phone_after_start() {
  s_prefs.begin("phones", false);
  esp_hf_client_register_callback(hf_event);
  esp_err_t e = esp_hf_client_init();
  if (e == ESP_OK) e = esp_hf_client_register_data_callback(hf_audio_in, hf_audio_out);
  Serial.printf("hfp: hands-free client %s\n", esp_err_to_name(e));
  list_load();
  s_pair_auto = s_count == 0;
  s_cod_at_ms = millis() + 1000;  // after the sink's stack-up work (profiles add their own service bits)
}

void phone_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_CFM_REQ_EVT:
    case ESP_BT_GAP_KEY_NOTIF_EVT: {
      const uint8_t *bda = event == ESP_BT_GAP_CFM_REQ_EVT ? param->cfm_req.bda : param->key_notif.bda;
      const uint32_t key = event == ESP_BT_GAP_CFM_REQ_EVT ? param->cfm_req.num_val : param->key_notif.passkey;
      // Only while the window is open (a new phone), or for a phone already paired that pairs again.
      bool known = false;
      for (uint8_t i = 0; i < s_count; i++) known |= same(s_list[i].bda, bda);
      if (!pairing_window_open() && !known) {
        Serial.println("phones: pairing request outside the pairing window, refused");
        if (event == ESP_BT_GAP_CFM_REQ_EVT) esp_bt_gap_ssp_confirm_reply(const_cast<uint8_t *>(bda), false);
        break;
      }
      Serial.printf("phones: pairing code %06lu (%s, %s), at %lu ms\n", (unsigned long)key,
                    event == ESP_BT_GAP_CFM_REQ_EVT ? "confirm" : "shown only", known ? "already paired" : "new phone",
                    (unsigned long)millis());
      portENTER_CRITICAL(&s_mux);
      s_confirm = true;
      s_confirm_reply = event == ESP_BT_GAP_CFM_REQ_EVT;
      memcpy(s_confirm_bda, bda, ESP_BD_ADDR_LEN);
      s_passkey = key;
      s_confirm_until_ms = millis() + kConfirmTimeoutMs;
      default_name(bda, s_pairing_name);
      s_pairing_serial = (uint8_t)((s_pairing_serial + 1) & 0x0F);
      changed();
      portEXIT_CRITICAL(&s_mux);
      esp_bt_gap_read_remote_name(const_cast<uint8_t *>(bda));
      break;
    }
    case ESP_BT_GAP_KEY_REQ_EVT:
      // We have no keypad: a phone that wants a code typed in here cannot pair.
      esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
      break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
      // Legacy pairing (very old phones): fixed PIN 0000, only while the window is open.
      esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
      esp_bt_gap_pin_reply(param->pin_req.bda, pairing_window_open(), 4, pin);
      break;
    }
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      portENTER_CRITICAL(&s_mux);
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        s_new_bond = true;
        memcpy(s_new_bond_bda, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        copy_text(s_new_bond_name, reinterpret_cast<const char *>(param->auth_cmpl.device_name));
      }
      s_confirm = false;
      changed();
      portEXIT_CRITICAL(&s_mux);
      // Link key type: 4/5 unauthenticated / authenticated (P-192), 7/8 the same with P-256 (Secure Connections).
      Serial.printf("phones: pairing %s (%s), status %d, link key type %d, at %lu ms\n",
                    param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? "done" : "failed",
                    reinterpret_cast<const char *>(param->auth_cmpl.device_name), (int)param->auth_cmpl.stat,
                    (int)param->auth_cmpl.lk_type, (unsigned long)millis());
      break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
      Serial.printf("phones: link up %02x:%02x:%02x:%02x:%02x:%02x, status %d, at %lu ms\n", param->acl_conn_cmpl_stat.bda[0],
                    param->acl_conn_cmpl_stat.bda[1], param->acl_conn_cmpl_stat.bda[2], param->acl_conn_cmpl_stat.bda[3],
                    param->acl_conn_cmpl_stat.bda[4], param->acl_conn_cmpl_stat.bda[5], (int)param->acl_conn_cmpl_stat.stat,
                    (unsigned long)millis());
      break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
      Serial.printf("phones: link down, reason 0x%02x, at %lu ms\n", (unsigned)param->acl_disconn_cmpl_stat.reason,
                    (unsigned long)millis());
      break;
    case ESP_BT_GAP_ENC_CHG_EVT:
      Serial.printf("phones: encryption mode %d, at %lu ms\n", (int)param->enc_chg.enc_mode, (unsigned long)millis());
      break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
      if (param->read_rmt_name.stat != ESP_BT_STATUS_SUCCESS) break;
      portENTER_CRITICAL(&s_mux);
      if (s_confirm && same(s_confirm_bda, param->read_rmt_name.bda)) {
        copy_text(s_pairing_name, reinterpret_cast<const char *>(param->read_rmt_name.rmt_name));
        s_pairing_serial = (uint8_t)((s_pairing_serial + 1) & 0x0F);
        changed();
      }
      s_named = true;
      memcpy(s_named_bda, param->read_rmt_name.bda, ESP_BD_ADDR_LEN);
      copy_text(s_named_name, reinterpret_cast<const char *>(param->read_rmt_name.rmt_name));
      portEXIT_CRITICAL(&s_mux);
      break;
    default:
      break;
  }
}

void phone_a2dp_connection(esp_a2d_connection_state_t state, const uint8_t *bda) {
  if (state != ESP_A2D_CONNECTION_STATE_CONNECTED && state != ESP_A2D_CONNECTION_STATE_DISCONNECTED) return;
  portENTER_CRITICAL(&s_mux);
  s_a2dp = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
  if (bda) memcpy(s_a2dp_bda, bda, ESP_BD_ADDR_LEN);
  if (s_a2dp) {
    s_a2dp_since_ms = millis();
    s_touch = true;
    memcpy(s_touch_bda, s_a2dp_bda, ESP_BD_ADDR_LEN);
  }
  changed();
  portEXIT_CRITICAL(&s_mux);
}

void phone_loop() {
  const uint32_t now = millis();
  if (s_cod_at_ms && (int32_t)(now - s_cod_at_ms) >= 0) {
    s_cod_at_ms = 0;
    set_class_of_device();
  }

  // Hand-offs from the callbacks.
  bool nb, touch, named;
  esp_bd_addr_t nb_bda, touch_bda, named_bda;
  char nb_name[kPhoneTextMax + 1], named_name[kPhoneTextMax + 1];
  portENTER_CRITICAL(&s_mux);
  nb = s_new_bond;
  touch = s_touch;
  named = s_named;
  s_new_bond = s_touch = s_named = false;
  memcpy(nb_bda, s_new_bond_bda, ESP_BD_ADDR_LEN);
  memcpy(nb_name, s_new_bond_name, sizeof(nb_name));
  memcpy(touch_bda, s_touch_bda, ESP_BD_ADDR_LEN);
  memcpy(named_bda, s_named_bda, ESP_BD_ADDR_LEN);
  memcpy(named_name, s_named_name, sizeof(named_name));
  portEXIT_CRITICAL(&s_mux);
  if (nb) {
    list_to_front(nb_bda, nb_name);
    s_pair_until_ms = 0;  // a phone paired: close the window
    s_pair_auto = false;
  }
  if (touch) {
    const bool is_new = list_find(touch_bda) < 0;
    list_to_front(touch_bda, nullptr);
    if (is_new) esp_bt_gap_read_remote_name(touch_bda);
  }
  if (named) {
    const int i = list_find(named_bda);
    if (i >= 0 && named_name[0] && strcmp(named_name, s_list[i].name) != 0) {
      portENTER_CRITICAL(&s_mux);
      copy_text(s_list[i].name, named_name);
      portEXIT_CRITICAL(&s_mux);
      list_changed();
    }
  }
  if (s_list_dirty) list_save();

  // Pairing window and the code waiting for an answer.
  if (s_pair_until_ms && (int32_t)(now - s_pair_until_ms) >= 0) s_pair_until_ms = 0;
  apply_scan_mode();
  bool expire = false;
  esp_bd_addr_t cbda;
  portENTER_CRITICAL(&s_mux);
  if (s_confirm && (int32_t)(now - s_confirm_until_ms) >= 0) {
    expire = s_confirm_reply;
    memcpy(cbda, s_confirm_bda, ESP_BD_ADDR_LEN);
    s_confirm = false;
    changed();
  }
  portEXIT_CRITICAL(&s_mux);
  if (expire) esp_bt_gap_ssp_confirm_reply(cbda, false);

  // Media and calls come from the same phone: bring up whichever link the phone did not.
  bool a2dp, slc, hfp_idle, want_clcc;
  uint32_t a2dp_since, hfp_since;
  esp_bd_addr_t a2dp_bda, hfp_bda;
  portENTER_CRITICAL(&s_mux);
  a2dp = s_a2dp;
  slc = s_slc;
  hfp_idle = s_hfp_state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED;
  a2dp_since = s_a2dp_since_ms;
  hfp_since = s_hfp_since_ms;
  memcpy(a2dp_bda, s_a2dp_bda, ESP_BD_ADDR_LEN);
  memcpy(hfp_bda, s_hfp_bda, ESP_BD_ADDR_LEN);
  want_clcc = s_want_clcc && slc;
  if (want_clcc) s_want_clcc = false;
  portEXIT_CRITICAL(&s_mux);
  if (!a2dp) s_hfp_tries = 0;
  if (a2dp && !slc && hfp_idle && now - a2dp_since > 2000 && s_hfp_tries < 3 && now - s_hfp_try_ms > 5000) {
    s_hfp_try_ms = now;
    s_hfp_tries++;
    Serial.println("hfp: connecting to the media phone");
    esp_hf_client_connect(a2dp_bda);
  }
  if (!slc) s_a2dp_tried = false;
  if (slc && !a2dp && !s_a2dp_tried && now - hfp_since > 3000) {
    s_a2dp_tried = true;
    Serial.println("a2dp: connecting to the hands-free phone");
    s_sink->connect_to(hfp_bda);
  }
  if (want_clcc) esp_hf_client_query_current_calls();

  // Connect to a chosen phone: drop the current one first.
  if (s_target) {
    esp_bd_addr_t cur;
    const bool conn = connected_bda(cur);
    if (conn && same(cur, s_target_bda)) {
      s_target = false;  // done (the other link follows above)
    } else if (now - s_target_ms > kConnectTimeoutMs) {
      Serial.println("phones: connect timed out");
      s_target = false;
    } else if (conn) {
      if (!s_target_dropped) {
        s_target_dropped = true;
        disconnect_all();
      }
    } else if (!s_target_dialed) {
      s_target_dialed = true;
      s_sink->connect_to(s_target_bda);
      esp_hf_client_connect(s_target_bda);
    }
  }
}

bool phone_command(uint8_t cmd, uint8_t arg, uint8_t list_serial, bool check_serial) {
  esp_bd_addr_t cur;
  const bool conn = connected_bda(cur);
  bool slc, ok = true;
  portENTER_CRITICAL(&s_mux);
  slc = s_slc;
  portEXIT_CRITICAL(&s_mux);
  const bool index_ok = arg < s_count && (!check_serial || list_serial == s_list_serial);

  switch (cmd) {
    case kPhoneCmdAnswer: ok = slc && esp_hf_client_answer_call() == ESP_OK; break;
    case kPhoneCmdEnd: ok = slc && esp_hf_client_reject_call() == ESP_OK; break;  // AT+CHUP: decline or hang up
    case kPhoneCmdMicMute:
      portENTER_CRITICAL(&s_mux);
      s_mic_muted = arg != 0;
      portEXIT_CRITICAL(&s_mux);
      call_audio_set_mute(arg != 0);
      break;
    case kPhoneCmdCallVolume: {
      const uint8_t v = arg > 15 ? 15 : arg;
      call_audio_set_volume(v);
      portENTER_CRITICAL(&s_mux);
      s_call_vol = v;
      portEXIT_CRITICAL(&s_mux);
      if (slc) esp_hf_client_volume_update(ESP_HF_VOLUME_CONTROL_TARGET_SPK, v);  // keeps the phone's slider in step
      break;
    }
    case kPhoneCmdPairing:
      s_pair_until_ms = arg ? millis() + kPairWindowMs : 0;
      if (!arg) s_pair_auto = false;  // Stop means stop, even with nothing paired (the console must not get stuck on it)
      break;
    case kPhoneCmdConfirm: {
      bool reply = false;
      esp_bd_addr_t b;
      portENTER_CRITICAL(&s_mux);
      if (s_confirm) {
        reply = s_confirm_reply;
        memcpy(b, s_confirm_bda, ESP_BD_ADDR_LEN);
        s_confirm = false;
      } else {
        ok = false;
      }
      portEXIT_CRITICAL(&s_mux);
      if (reply) esp_bt_gap_ssp_confirm_reply(b, arg != 0);
      break;
    }
    case kPhoneCmdConnect:
      if (!index_ok) {
        ok = false;
        break;
      }
      memcpy(s_target_bda, s_list[arg].bda, ESP_BD_ADDR_LEN);
      s_target = true;
      s_target_ms = millis();
      s_target_dropped = s_target_dialed = false;
      Serial.printf("phones: connecting to %s\n", s_list[arg].name);
      break;
    case kPhoneCmdDisconnect:
      s_target = false;
      disconnect_all();
      break;
    case kPhoneCmdForget: {
      if (!index_ok) {
        ok = false;
        break;
      }
      esp_bd_addr_t b;
      memcpy(b, s_list[arg].bda, ESP_BD_ADDR_LEN);
      Serial.printf("phones: forgetting %s\n", s_list[arg].name);
      if (conn && same(cur, b)) disconnect_all();
      if (s_target && same(s_target_bda, b)) s_target = false;
      esp_bt_gap_remove_bond_device(b);
      if (s_sink && s_sink->get_last_peer_address() && same(*s_sink->get_last_peer_address(), b)) {
        s_sink->clean_last_connection();  // or the sink would keep reconnecting to it at boot
      }
      list_remove(arg);
      if (s_count == 0) s_pair_auto = true;  // the last phone is gone: be findable again
      break;
    }
    case kPhoneCmdVoice:
      ok = slc && (arg ? esp_hf_client_start_voice_recognition() : esp_hf_client_stop_voice_recognition()) == ESP_OK;
      break;
    case kPhoneCmdRedial: ok = slc && esp_hf_client_dial(nullptr) == ESP_OK; break;
    case kPhoneCmdCallWaiting: {
      const esp_hf_chld_type_t t = arg == 0 ? ESP_HF_CHLD_TYPE_REL : (arg == 1 ? ESP_HF_CHLD_TYPE_REL_ACC : ESP_HF_CHLD_TYPE_HOLD_ACC);
      ok = slc && esp_hf_client_send_chld_cmd(t, 0) == ESP_OK;
      break;
    }
    case kPhoneCmdAudioRoute:
      ok = slc && (arg ? esp_hf_client_connect_audio(cur) : esp_hf_client_disconnect_audio(cur)) == ESP_OK;
      break;
    default: ok = false; break;
  }
  portENTER_CRITICAL(&s_mux);
  changed();  // the bridge answers every command with a fresh status
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void phone_get(PhoneState *o) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&s_mux);
  esp_bd_addr_t cur;
  const bool conn = connected_bda_locked(cur);
  o->flags = (uint8_t)((conn ? kPhoneConnected : 0) | (s_slc ? kPhoneHfp : 0) | (s_sco ? kPhoneCallAudio : 0) |
                       (s_mic_muted ? kPhoneMicMuted : 0) | (s_scan_open ? kPhonePairing : 0) |
                       (s_confirm ? kPhoneConfirm : 0) | (s_inband ? kPhoneInbandRing : 0) | (s_voice ? kPhoneVoice : 0));
  o->call = s_call;
  const uint32_t cs = s_call_start_ms ? (now - s_call_start_ms) / 1000 : 0;
  o->call_s = (uint16_t)(cs > 65535 ? 65535 : cs);
  o->signal = s_signal > 5 ? kPhoneNone : s_signal;
  o->battery = s_battery > 5 ? kPhoneNone : s_battery;
  o->caller_serial = s_caller_serial;
  o->list_serial = s_list_serial;
  o->pairing_serial = s_pairing_serial;
  o->paired = s_count;
  o->connected_index = kPhoneNone;
  o->connected_name[0] = '\0';
  for (uint8_t i = 0; i < s_count; i++) {
    memcpy(o->paired_name[i], s_list[i].name, sizeof(o->paired_name[i]));
    if (conn && same(s_list[i].bda, cur)) {
      o->connected_index = i;
      memcpy(o->connected_name, s_list[i].name, sizeof(o->connected_name));
    }
  }
  for (uint8_t i = s_count; i < kPhoneMaxPaired; i++) o->paired_name[i][0] = '\0';
  o->connected_serial = s_list_serial;  // the connected name changes only with the list (connecting moves it to the top)
  o->call_volume = s_call_vol;
  o->passkey = s_passkey;
  if (s_pair_auto) {
    o->pair_left = kPhonePairNoLimit;
  } else if (s_pair_until_ms) {
    const uint32_t left = (s_pair_until_ms - now + 999) / 1000;
    o->pair_left = (uint8_t)(left > 254 ? 254 : left);
  } else {
    o->pair_left = 0;
  }
  memcpy(o->caller, s_caller, sizeof(o->caller));
  memcpy(o->pairing_name, s_pairing_name, sizeof(o->pairing_name));
  portEXIT_CRITICAL(&s_mux);
}

uint32_t phone_changes() { return s_changes; }

void phone_print_status() {
  static const char *kCall[] = {"idle", "incoming", "dialing", "alerting", "active", "waiting", "held"};
  PhoneState st;
  phone_get(&st);
  Serial.printf("phone: %s%s%s%s%s%s, call %s %us, caller \"%s\", signal %u battery %u, call volume %u\n",
                (st.flags & kPhoneConnected) ? "connected" : "not connected", (st.flags & kPhoneHfp) ? " +hands-free" : "",
                (st.flags & kPhoneCallAudio) ? " +call audio" : "", (st.flags & kPhoneMicMuted) ? " muted" : "",
                (st.flags & kPhoneInbandRing) ? " in-band ring" : "", (st.flags & kPhoneVoice) ? " voice" : "",
                kCall[st.call < 7 ? st.call : 0], (unsigned)st.call_s, st.caller, (unsigned)st.signal,
                (unsigned)st.battery, (unsigned)st.call_volume);
  if (st.flags & kPhonePairing) {
    if (st.pair_left == kPhonePairNoLimit) Serial.println("pairing: open (no phone paired yet)");
    else Serial.printf("pairing: open, %us left\n", (unsigned)st.pair_left);
  }
  if (st.flags & kPhoneConfirm) Serial.printf("pairing: \"%s\" shows %06lu? (confirm yes|no)\n", st.pairing_name, (unsigned long)st.passkey);
  Serial.printf("paired (list serial %u):\n", (unsigned)st.list_serial);
  for (uint8_t i = 0; i < st.paired; i++) {
    Serial.printf("  %u: %s%s\n", (unsigned)i, st.paired_name[i], i == st.connected_index ? "  (connected)" : "");
  }
  char a[200];
  call_audio_status(a, sizeof(a));
  Serial.printf("call audio: %s\n", a);
}
