#pragma once

#include <Arduino.h>
#include <stdint.h>

// Matches src/dashboard/input/arduino_serial.py and arduino/sketches/sensor_pack/sensor_pack.ino
//
// UART wiring (all three ESP32-P4 gauge boards use the same pins by default):
//   sensor_pack TX →  ESP32 UART RX pin (SENSOR_UART_RX, default GPIO 5) — packets in
//   sensor_pack RX  ←  ESP32 UART TX pin (SENSOR_UART_TX, default GPIO 4) — 0xA5 handshake out
// Do not swap these relative to the ATmega: TX must meet RX.
//
// On the Waveshare 40-pin header, use the silkscreen numbers "5" and "4" (GPIO 5 / GPIO 4), not RXD/TXD.
//
// ATmega sensor_pack uses Serial (115200): on Uno/Nano that is **D1 = TX** and **D0 = RX**.
// Do **not** confuse with sketch pins D4/D5 (alternator / RPM-select inputs — not UART).
static const uint8_t SENSOR_SYNC0 = 0xAA;
static const uint8_t SENSOR_SYNC1 = 0x55;
static const uint8_t PI_HANDSHAKE_BYTE = 0xA5;
static const size_t LEGACY_PAYLOAD_LEN = 19;
static const size_t EXTENDED_PAYLOAD_LEN = 21;
// v3: extended payload + big-endian odometer in tenths of a mile (sensor_pack source of truth).
static const size_t ODOMETER_PAYLOAD_LEN = 25;
// v4: v3 + instant/trip MPG (x10) + DTE in tenths of a mile (20 gal tank model on sensor_pack).
static const size_t ECONOMY_PAYLOAD_LEN = 33;

struct SensorPacketValues {
  uint16_t pot_adc;
  uint16_t therm_adc;
  uint16_t pressure_adc;
  uint16_t voltage_adc;
  uint16_t ambient_adc;
  uint32_t speed_rpm;
  uint32_t engine_rpm;
  uint8_t flags;
  uint16_t faults;
  uint32_t odometer_tenths_mile;
  bool has_odometer;
  uint16_t mpg_instant_x10;
  uint16_t mpg_average_x10;
  uint32_t dte_tenths_mile;
  bool has_fuel_economy;
  bool extended;
  bool valid;
};

struct SensorDerived {
  float fuel_pct;      // 0..100
  float oil_psi;       // mapped like Pi (0..100 scale)
  float coolant_temp_f;
  float system_amp;    // -100..100 from voltage (amp gauge)
  bool valid;
};

class SensorPacketParser {
 public:
  void begin(HardwareSerial &serial) { _ser = &serial; }
  bool poll(SensorPacketValues &out, bool send_handshake_on_good);

  /** Pop next UART text line (e.g. sensor_pack boot banner) stripped before binary frames; cap includes NUL. */
  bool takePendingTextLine(char *out, size_t cap);

  static SensorDerived derive(const SensorPacketValues &p);

 private:
  HardwareSerial *_ser = nullptr;
  uint8_t _buf[1024];
  size_t _len = 0;

  void stripLeadingUartTextLines();

  static uint8_t xor_payload(const uint8_t *p, size_t n);
  static bool parse_payload(const uint8_t *payload, size_t len, SensorPacketValues &out);
};
