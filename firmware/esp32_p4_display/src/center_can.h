#pragma once

#include <stddef.h>
#include <stdint.h>

#include <elc_can_codec.h>
#include <sensor_packet.h>

// Center's CAN node (docs/protocols/can_bus.md): re-sends each sensor pack frame and the cluster
// state, takes setting requests from the other screens, and
// feeds the audio bridge's frames to media_can. Pins: ELC_CAN_TX_PIN /
// ELC_CAN_RX_PIN (GPIO34 / GPIO36 on the DigiDash board; -1 = off).

void center_can_begin();
void center_can_poll();

/** Sends one sensor frame as CAN 0x100..0x104. */
void center_can_send_sensor(const SensorPacketValues &v);
/** Cluster state (CAN 0x210): theme, center view, interface / media screens, brightness. On change and every 5 s. */
void center_can_send_cluster(const ElcCanClusterState &st);
/** Center status (0x213), about once a second. */
void center_can_send_status(const ElcCanCenterStatus &st);
/** Wall time (0x220), once a second. */
void center_can_send_time(const ElcCanTime &t);
/** Put one frame on the bus (false when CAN is off or the queue is full). */
bool center_can_send(const ElcCanFrame &f);
/** Next setting request (0x212) from another screen or the console; false when none is waiting. */
bool center_can_take_setting(ElcCanSettingRequest *out);

// The audio bridge (now playing, album art; phone) is handled by media_can / phone_can (esp32_p4_ui), and the
// console's menu input by menu_can, all fed from here.

/** Defined by center_main: sensor frames heard on the bus (a USB relay), used while the sensor UART is silent. */
void center_sensor_from_bus(const ElcCanSensor &s);

/** A heartbeat from this node within the last 3 s. */
bool center_can_node_alive(uint8_t node);

/** Bench console summary (`can`). */
void center_can_describe(char *out, size_t n);
