#pragma once

#include <Arduino.h>

#include <demo_drive.h>
#include <sensor_packet.h>

// A frame older than this counts as "no data" (sensor_pack sends every 50 ms).
#ifndef SENSOR_PACKET_STALE_MS
#define SENSOR_PACKET_STALE_MS 400u
#endif

// Only ONE board should send the 0xA5 keepalive back to the sensor pack (the center, by default):
// three push-pull TX pins tied to one RX would fight. Boards with this 0 never write to the UART.
#ifndef SENSOR_LINK_KEEPALIVE
#define SENSOR_LINK_KEEPALIVE 0
#endif
#ifndef SENSOR_LINK_KEEPALIVE_MS
#define SENSOR_LINK_KEEPALIVE_MS 350u
#endif

/**
 * Owns the sensor_pack UART on a gauge board: parses frames, keeps the last good one, tracks
 * staleness, and (optionally) sends the keepalive byte. Replaces the per-app copies of this logic.
 */
class SensorLink {
 public:
  void begin(HardwareSerial &ser, int rx_pin, int tx_pin);

  /** Drain the UART; returns true if at least one new frame arrived. Safe to call several times per loop. */
  bool poll();

  /** A frame arrived within SENSOR_PACKET_STALE_MS. */
  bool fresh() const;
  bool everReceived() const { return _have; }
  uint32_t ageMs() const { return _have ? (millis() - _last_ms) : UINT32_MAX; }

  /** Last good frame (may be stale — check fresh()). */
  const SensorPacketValues &last() const { return _held; }

  /** Derived gauge values; when stale, valid=false and values are at rest (0 / cold). */
  SensorDerived derived() const;

  /** Odometer from the sensor pack, if fresh and known. */
  bool odometerMiles(float *out) const;

  /** Frames received in the last full second. */
  uint32_t framesPerSec() const { return _fps; }
  const SensorPacketParser &parser() const { return _parser; }

  bool takeTextLine(char *out, size_t cap) { return _parser.takePendingTextLine(out, cap); }

  /** Bench demo: replace sensor-pack frames with a simulated 60 s drive cycle (20 Hz). The UART is still
   *  drained so nothing piles up; real frames are ignored until demo is switched off. */
  void setDemo(bool on);
  bool demo() const { return _demo; }

 private:
  DemoDrive _drive;
  uint32_t _fps = 0;
  uint32_t _fps_count = 0;
  uint32_t _fps_t0 = 0;
  bool _demo = false;
  uint32_t _demo_next_ms = 0;
  SensorPacketParser _parser;
  HardwareSerial *_ser = nullptr;
  SensorPacketValues _held{};
  uint32_t _last_ms = 0;
  uint32_t _last_keepalive_ms = 0;
  bool _have = false;
};
