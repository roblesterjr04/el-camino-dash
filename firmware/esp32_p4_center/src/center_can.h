#pragma once

#include <stddef.h>
#include <stdint.h>

#include <sensor_packet.h>

// Center's CAN node (docs/protocols/can_bus.md): re-sends each sensor pack frame and the cluster
// state, and listens for the accessory slots (audio bridge now-playing). Pins: ELC_CAN_TX_PIN /
// ELC_CAN_RX_PIN (GPIO34 / GPIO36 on the DigiDash board; -1 = off).

void center_can_begin();
void center_can_poll();

/** Sends one sensor frame as CAN 0x100..0x104. */
void center_can_send_sensor(const SensorPacketValues &v);
/** Theme + center sub-view (CAN 0x210); called with every cluster I2C broadcast. */
void center_can_send_cluster(uint8_t theme, uint8_t center_subview);

/** Now playing from an audio bridge on the CAN bus, if heard within max_age_ms. line gets "Artist - Title". */
bool center_can_audio(char *line, size_t n, uint16_t *elapsed_s, uint16_t *duration_s, uint8_t *vol_0_15,
                      uint32_t max_age_ms = 2000);
/** Slot of the bridge last heard and the album art id it announces (0 = none ready). */
bool center_can_audio_art(uint8_t *slot, uint8_t *art_id, uint32_t max_age_ms = 2000);
/** Ask the bridge in `slot` for `variant` of album art `art_id`, starting at chunk `first` (docs/protocols/can_bus.md). */
bool center_can_request_art(uint8_t slot, uint8_t art_id, uint8_t variant, uint16_t first);

/** Bench console summary (`can`). */
void center_can_describe(char *out, size_t n);
