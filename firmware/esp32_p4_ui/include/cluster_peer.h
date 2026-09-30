#pragma once

#include <stdint.h>

#include <elc_can_codec.h>

class SensorLink;

/**
 * A side screen's part in the cluster (left speedometer, right gauges). The center owns the settings; this
 * module follows them from CAN (0x210 cluster state, 0x213 / 0x214 center status) and asks for changes with
 * setting requests (0x212):
 *   - interface screen = this one: a tap opens the menu here (ui_menu.h, backed by CAN); otherwise the
 *     menu stays closed here;
 *   - media screen = this one: a 3 s hold toggles the media view next to this screen's gauge;
 *   - brightness follows the center's setting.
 * It also feeds the audio bridge's frames to media_can and fetches album art while media is shown here.
 *
 * Loop contract: pass every CAN frame to cluster_peer_frame(); call cluster_peer_poll() once per loop with
 * the touch sample, before p4_screen_frame().
 */
struct ClusterPeerConfig {
  uint8_t self_screen = ELC_CAN_SCREEN_LEFT;  // ELC_CAN_SCREEN_*
  uint8_t node = ELC_CAN_NODE_SPEEDO;
  bool (*send)(const ElcCanFrame &f) = nullptr;
  /** CAN heartbeat seen from this node lately (Service page). */
  bool (*node_alive)(uint8_t node) = nullptr;
  const SensorLink *link = nullptr;
  /** This screen's own 3 s hold when it is not the media screen (e.g. the right screen's Classic layout);
   *  nullptr = none. hold_enabled says whether it applies now; hold_message is shown while holding. */
  void (*on_hold)() = nullptr;
  bool (*hold_enabled)() = nullptr;
  const char *hold_message = "Release: switch layout";
};

void cluster_peer_begin(const ClusterPeerConfig &cfg);

/** Feed one received frame (cluster state, center status and text, audio bridge). */
void cluster_peer_frame(const ElcCanFrame &f);

/** Touch, menu, hold gestures, brightness and the media fetch. */
void cluster_peer_poll(uint32_t now_ms, int16_t touch_x, int16_t touch_y, bool touch_down);

/** A cluster state from the center has been heard (else: defaults, center is the interface and media screen). */
bool cluster_peer_have_state();
const ElcCanClusterState &cluster_peer_state();

/** This screen shows its media view now (instead of its gauge). */
bool cluster_peer_show_media();

/** The center's status (0x213, 1 Hz) was heard within 3 s: a setting request will be answered. */
bool cluster_peer_center_heard();
/** Ask the center to change a setting (0x212, ElcCanSetting); it answers with a fresh 0x210. */
void cluster_peer_request(uint8_t setting, uint8_t value);
