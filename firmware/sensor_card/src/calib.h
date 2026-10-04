#pragma once

// Sensor calibration (docs/protocols/sensor_calibration.md): how ADC counts become litres, degrees and kilopascals,
// what is stored in the FRAM, and the capture rules. No Arduino dependencies: host-tested (test/host/run_tests.sh).
//
// Everything is stored in canonical units: fuel in litres x10, temperatures in degC x10, oil pressure in kPa, battery
// in volts. The menu converts to the center's units (gal, F, psi) on the way in and out (card_ui.cpp).

#include <stddef.h>
#include <stdint.h>

namespace cal {

enum Channel : uint8_t { FUEL = 0, TEMP = 1, OIL = 2, AMB = 3 };
enum Topology : uint8_t { PULL_UP = 0, PULL_DOWN = 1 };
// SENSOR status codes (can_channels.md)
enum Status : uint8_t { ST_OK = 0, ST_NOT_READY = 1, ST_OPEN = 2, ST_SHORT = 3, ST_RANGE = 4, ST_FAULT = 5 };

static const uint8_t kFuelPresets = 6;  // 0 none, 1 GM 0-30, 2 GM 0-90, 3 Ford 73-10, 4 240-33, 5 VDO 10-180
static const uint8_t kTempPresets = 4;  // 0 none, 1 100k NTC, 2 10k NTC, 3 GM 2-wire coolant sensor
static const uint8_t kMaxPoints = 8;

/** Points a channel keeps (fuel 8, temperature 6, oil 4). */
inline uint8_t max_points(uint8_t ch) { return ch == FUEL ? 8 : ch == TEMP ? 6 : 4; }

struct Point {
  uint16_t raw = 0;
  int16_t value = 0;  // fuel L x10, temperature degC x10, oil kPa
};

/** One sender channel's settings: must match the card's jumpers (it cannot read them). */
struct ChannelCal {
  uint8_t topology = PULL_UP;
  uint32_t bias_x10 = 1000;  // the bridged bank value (or the parallel value), ohms x10 (u32: 100k is 1 000 000)
  uint8_t preset = 0;
  int16_t offset = 0;        // added last; temperature channels, degC x10
  uint8_t n = 0;
  Point pts[kMaxPoints];     // sorted by raw
};

struct Calibration {
  uint16_t flags = 1;  // bit0: at factory defaults (nothing changed yet)
  ChannelCal fuel, temp, oil;
  uint8_t amb_ntc = 0;          // 0 100k NTC (JP16), 1 10k NTC (JP15)
  int16_t amb_offset = 0;       // degC x10
  uint16_t tank_x10 = 757;      // litres x10: 20.0 gal, the 1964-67 El Camino tank
  int32_t batt_trim_ppm = 0;    // +-50 000 = +-5 %
  uint32_t speed_ppm_x100 = 800000;  // speed pulses per mile x100 (8000: a common cable-drive replacement sensor)
  uint8_t tach_ppr = 0;         // pulses per revolution, 0 = from the J5 jumpers
};

static const uint16_t kFlagDefaults = 1;

/** The card as delivered: fuel pull-up 100 ohm with the GM 0-30 ohm preset; temperature pull-up 1k and oil pull-up 220
 *  ohm with no preset (not calibrated: they read `--` on purpose); ambient 100k NTC. */
void defaults(Calibration &c);

ChannelCal &channel(Calibration &c, uint8_t ch);
const ChannelCal &channel(const Calibration &c, uint8_t ch);

/** Sender ohms from counts (topology and bias applied). */
float raw_to_ohms(const ChannelCal &c, float raw);
/** Counts for a sender resistance. */
float ohms_to_raw(const ChannelCal &c, float ohms);

/** Temperature for a thermistor resistance. `r0` is its resistance at 25 C (Beta 3950). */
float ntc_celsius(float ohms, float r0);
/** The GM 2-wire coolant sensor's published resistance chart, interpolated (degC). */
float gm_coolant_celsius(float ohms);
/** Fraction full (0..1) for a fuel preset at a sender resistance. */
float fuel_fraction(uint8_t preset, float ohms);

struct Reading {
  int32_t value = 0;  // fuel L x10, temperature degC x10, oil kPa
  uint8_t status = ST_NOT_READY;
};

/** True when the channel can give a value: two points or more, or a preset. */
bool calibrated(const Calibration &c, uint8_t ch);
/** counts (0..1023, the averaged reading) to the channel's value and status (open / short per the spec). */
Reading convert(const Calibration &c, uint8_t ch, float raw);
/** The ambient NTC. Always calibrated (a formula). */
Reading convert_ambient(const Calibration &c, float raw);
/** Battery volts from the average counts of ADC8 (100k / 22k divider, 5 V reference), trim applied. */
float battery_volts(const Calibration &c, float raw);
/** The trim (ppm) that makes `raw` read `meter_volts`. */
int32_t battery_trim_for(float raw, float meter_volts);

/** Adds a captured point. A point within 3 counts of an existing one replaces it; a full table replaces the nearest.
 *  False (and the table unchanged) when the result would not be monotonic. */
bool capture(ChannelCal &c, uint8_t ch, uint16_t raw, int16_t value);
/** Raw and value both rising, or one rising as the other falls, all the way along. */
bool monotonic(const ChannelCal &c);
void clear_points(ChannelCal &c);

/** The calibration blob (payload, version 1, little-endian). Returns the length, 0 if `cap` is too small. */
static const size_t kPayloadMax = 128;
size_t serialize(const Calibration &c, uint8_t *out, size_t cap);
/** Missing trailing bytes take the defaults, unknown trailing bytes are ignored. False if it is too short to be one. */
bool deserialize(const uint8_t *p, size_t n, Calibration &c);

/** A stored copy: `CA 1B`, version, reserved, sequence u32, payload length u16, payload, CRC-16 over all before it. */
static const size_t kBlockMax = 12 + kPayloadMax;
size_t block_encode(const Calibration &c, uint32_t seq, uint8_t *out, size_t cap);
bool block_decode(const uint8_t *p, size_t n, uint32_t *seq, Calibration &c);

}  // namespace cal
