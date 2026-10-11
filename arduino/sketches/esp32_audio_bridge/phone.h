#pragma once

/*
 * Phones: hands-free calls (HFP client) and phone management (pairing, the paired list, connect / forget).
 *
 * - HFP: call state from the phone's indicators (+CIND), the caller from +CLIP / +CCWA / +CLCC, answer / end /
 *   call waiting / redial / voice assistant, speaker volume, microphone mute. Call audio: audio_engine.h.
 * - Pairing: numeric comparison (the phone and the console show the same six-digit code; the user confirms on the
 *   console). The bridge is discoverable only while the pairing window is open: on command for 2 minutes, and with no
 *   time limit while nothing is paired (from boot, or from the last Forget, until a phone pairs or the console says
 *   Stop). A pairing request from an unknown phone outside the window is refused.
 * - Paired list: up to kPhoneMaxPaired phones, most recently connected first, with their names, kept in NVS next to
 *   Bluedroid's own bond store. Pairing a ninth phone forgets the oldest.
 *
 * The numbering (flags, call states, commands) is the CAN protocol's (ELC_CAN_PHONE_*, ElcCanCallState,
 * ElcCanPhoneCommand in elc_can_codec.h; docs/protocols/can_bus.md, "Phone"). This module does not depend on the CAN
 * library so the plain arduino-cli build keeps working; the sketch checks the numbers match.
 *
 * Threading: the GAP / A2DP / HFP callbacks run on Bluetooth tasks and only update state; NVS writes, reconnects and
 * the pairing window are handled in phone_loop() on the loop task.
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_a2dp_api.h"
#include "esp_gap_bt_api.h"

class BluetoothA2DPSink;

static constexpr uint8_t kPhoneMaxPaired = 8;
static constexpr size_t kPhoneTextMax = 32;
static constexpr uint8_t kPhoneNone = 0x0F;
static constexpr uint8_t kPhonePairNoLimit = 0xFF;  // pair_left: open with no time limit (nothing paired yet)

enum : uint8_t {
  kPhoneConnected = 0x01,
  kPhoneHfp = 0x02,
  kPhoneCallAudio = 0x04,
  kPhoneMicMuted = 0x08,
  kPhonePairing = 0x10,
  kPhoneConfirm = 0x20,
  kPhoneInbandRing = 0x40,
  kPhoneVoice = 0x80,
};

enum : uint8_t {
  kCallIdle = 0,
  kCallIncoming = 1,
  kCallDialing = 2,
  kCallAlerting = 3,
  kCallActive = 4,
  kCallWaiting = 5,
  kCallHeld = 6,
};

enum : uint8_t {
  kPhoneCmdAnswer = 1,
  kPhoneCmdEnd = 2,
  kPhoneCmdMicMute = 3,
  kPhoneCmdCallVolume = 4,
  kPhoneCmdPairing = 5,
  kPhoneCmdConfirm = 6,
  kPhoneCmdConnect = 7,
  kPhoneCmdDisconnect = 8,
  kPhoneCmdForget = 9,
  kPhoneCmdVoice = 10,
  kPhoneCmdRedial = 11,
  kPhoneCmdCallWaiting = 12,
  kPhoneCmdAudioRoute = 13,
  kPhoneCmdMedia = 14,        // handled by the sketch (it owns the A2DP sink's transport)
  kPhoneCmdMediaVolume = 15,  // handled by the sketch
};

struct PhoneState {
  uint8_t flags = 0;
  uint8_t call = kCallIdle;
  uint16_t call_s = 0;
  uint8_t signal = kPhoneNone, battery = kPhoneNone;
  uint8_t caller_serial = 0, list_serial = 0, connected_serial = 0, pairing_serial = 0;
  uint8_t paired = 0;
  uint8_t connected_index = kPhoneNone;
  uint8_t call_volume = 0;
  uint32_t passkey = 0;
  uint8_t pair_left = 0;  // seconds; kPhonePairNoLimit = open until a phone pairs
  char caller[kPhoneTextMax + 1] = {0};
  char connected_name[kPhoneTextMax + 1] = {0};
  char pairing_name[kPhoneTextMax + 1] = {0};
  char paired_name[kPhoneMaxPaired][kPhoneTextMax + 1] = {{0}};
};

/** Before sink->start(): pairing with confirmation, not discoverable by default. */
void phone_begin(BluetoothA2DPSink *sink);
/** After sink->start() (Bluedroid is up): HFP client, class of device, the paired list. */
void phone_after_start();
/** Loop task, every pass: pairing window, confirmation timeout, reconnects, NVS. */
void phone_loop();

/** From the sink's GAP callback (pairing) and A2DP connection events (which phone is connected). */
void phone_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
void phone_a2dp_connection(esp_a2d_connection_state_t state, const uint8_t *bda);

/**
 * A command (kPhoneCmd*). Commands that name a list index are ignored when list_serial is not the current one
 * (the list changed under the sender) unless check_serial is false (the serial console). False = not done.
 */
bool phone_command(uint8_t cmd, uint8_t arg, uint8_t list_serial, bool check_serial);

void phone_get(PhoneState *out);
/** Bumps on every change a listener should hear about at once. */
uint32_t phone_changes();

/** Multi-line description for the serial console. */
void phone_print_status();
