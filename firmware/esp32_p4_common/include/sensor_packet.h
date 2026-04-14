#pragma once

#include <Arduino.h>
#include <stdint.h>

// Matches src/dashboard/input/arduino_serial.py and arduino/sketches/sensor_pack/sensor_pack.ino
static const uint8_t SENSOR_SYNC0 = 0xAA;
static const uint8_t SENSOR_SYNC1 = 0x55;
static const uint8_t PI_HANDSHAKE_BYTE = 0xA5;
static const size_t LEGACY_PAYLOAD_LEN = 19;
static const size_t EXTENDED_PAYLOAD_LEN = 21;

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

  static SensorDerived derive(const SensorPacketValues &p);

 private:
  HardwareSerial *_ser = nullptr;
  uint8_t _buf[128];
  size_t _len = 0;

  static uint8_t xor_payload(const uint8_t *p, size_t n);
  static bool parse_payload(const uint8_t *payload, size_t len, SensorPacketValues &out);
};
