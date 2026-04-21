/*
 * sensor_pack.ino
 *
 * Reads sensors for a classic car gauge cluster with fault detection.
 * Transmits binary packets over hardware serial to a Raspberry Pi.
 *
 * Wiring notes:
 *
 *   A0 (potentiometer/fuel sender) supports two modes:
 *     - 3-wire potentiometer (5V, GND, wiper->A0): no external pull-down
 *     - 2-wire sender to 5V: add external pull-down on A0
 *
 *   A1/A2/A3 analog channels (engine temp, oil pressure, ambient) use a pull-down resistor:
 *
 *     5V
 *      |
 *     [Sensor]
 *      |
 *      +---- Ax (analog pin)
 *      |
 *     [470R - 1kR pull-down]
 *      |
 *     GND
 *
 *   This ensures (for pull-down channels):
 *     - Normal operation reads ~15 to ~1008
 *     - Open circuit (wire break) reads < LOW_THRESHOLD
 *     - Short to VCC reads > HIGH_THRESHOLD
 *     - Short to GND reads < LOW_THRESHOLD
 *
 * Sensors (fixed pins — move hardware to match):
 *   A0 - Variable resistor (fuel level or similar)
 *   A1 - Thermistor (engine temperature)
 *   A2 - Pressure sensor (oil pressure)
 *   A3 - Thermistor (ambient / outside temperature), same pull-down style as A1/A2
 *   FRAM (MB85RC, odometer) — hardware I2C via `Wire` @ 0x50 (100 kHz):
 *          ATmega328P: SDA/SCL = A4/A5 (same nets as the SDA/SCL pins by AREF on Uno-class boards).
 *          ATmega2560: SDA/SCL = D20/D21 (labeled SDA/SCL by AREF). Do not use A4/A5 for I2C on Mega.
 *   "Voltage" packet field — no A3 divider: synthesized charge/discharge ADC from D6 ign + D4 alt L.
 *   D2 - Hall effect sensor (vehicle speed)
 *   D3 - Ignition coil tach signal (engine RPM)
 *   D4 - Alternator "L" terminal (charge indicator)
 *   D5 - RPM selector bit 0 (grounded = LOW)
 *   D6 - Ignition sense (12V switched through voltage divider)
 *   D7 - Pot power monitor (sensor side of PTC fuse)
 *   D8 - Thermistor power monitor (sensor side of PTC fuse)
 *   D9 - Pressure power monitor (sensor side of PTC fuse)
 *   D10 - Voltage sense power monitor (sensor side of PTC fuse)
 *   D11 - Fault logging disable (HIGH = suppress fault reporting and send slow
 *         swept sample data for bench gauge testing)
 *   D12 - RPM selector bit 1 (grounded = LOW)
 *
 * Packet format (binary, big-endian), 36 bytes on wire (v4 fuel economy + v3 odometer):
 *   [0xAA] [0x55]          - sync header (2)
 *   [pot_hi] [pot_lo]      - variable resistor raw ADC (2)
 *   [temp_hi] [temp_lo]    - engine temp raw ADC (2)
 *   [pres_hi] [pres_lo]    - oil pressure raw ADC (2)
 *   [volt_hi] [volt_lo]    - charge/discharge gauge raw ADC (synthesized; not a shunt on A3) (2)
 *   [amb_hi] [amb_lo]      - ambient temp raw ADC (2)
 *   [spd_b3..spd_b0]       - vehicle speed RPM (4)
 *   [rpm_b3..rpm_b0]       - engine RPM (4)
 *   [flags]                - digital states (1)
 *   [faults_hi][faults_lo] - fault bitfield (2)
 *   [odo_b3..odo_b0]       - odometer: miles x10 (uint32 big-endian), source of truth (4)
 *   [mpg_i_hi][mpg_i_lo]    - instant MPG x10 (uint16 BE); 0 = invalid (2)
 *   [mpg_a_hi][mpg_a_lo]    - trip average MPG x10 since key-on (uint16 BE); 0 = n/a (2)
 *   [dte_b3..dte_b0]       - distance to empty, miles x10 (uint32 BE), 20 gal tank model (4)
 *   [crc]                  - XOR checksum (1)
 *                                            ------
 *                              Total:         36 bytes
 *
 * Odometer: integrated from wheel speed into miles (g_odo_tenths + g_odo_frac_miles). Each 0.1 mi
 * boundary bumps g_odo_tenths and stamps FRAM. Sub-0.1 mi progress is stored in FRAM (v2: 12-byte
 * blob with fractional part) and throttled ~2 s while moving so bench / short hall tests survive
 * reboot without crossing a full tenth. Legacy 8-byte FRAM is upgraded on load. Boot retries reads;
 * factory seed 96648.5 mi = 966485 tenths only if FRAM is missing or invalid. On boot, ASCII lines
 * (------ … ###### then 6-digit odometer tenths) print at 500 ms on Serial before binary packets.
 * FRAM rd/wr UART lines use short text (often two lines: status + t=…) so cluster UART parsers with small
 * line buffers still work; read summary is deferred until link (0xA5) or 6s so the center log sees it.
 * FRAM @ 0x50: hardware `Wire` only (100 kHz); pins follow the MCU (328P: A4/A5, Mega: 20/21). Set
 * SENSORPACK_ODOMETER_FRAM 0 to omit FRAM.
 */

#include <math.h>
#include <stdio.h>

#ifndef SENSORPACK_ODOMETER_FRAM
#define SENSORPACK_ODOMETER_FRAM 1
#endif
#if SENSORPACK_ODOMETER_FRAM
#include <Wire.h>
#endif

// Pin Assignments
#define PIN_POT        A0
#define PIN_THERMISTOR A1
#define PIN_PRESSURE   A2
#define PIN_AMBIENT_THERMISTOR A3
#define PIN_HALL       2
#define PIN_COIL       3
#define PIN_ALT_L      4
#define PIN_RPM_SEL_A  5
#define PIN_IGN_SENSE  6   // ignition on/off (through voltage divider)
#define PIN_POT_PWR_MON   7
#define PIN_TEMP_PWR_MON  8
#define PIN_PRESS_PWR_MON 9
#define PIN_VOLT_PWR_MON  10
#define PIN_FAULT_DISABLE  11
#define PIN_RPM_SEL_B      12
#define PIN_HANDSHAKE_LED  13

// Config
#define SERIAL_BAUD    115200
#define SEND_INTERVAL  50
// Demo sweep when D11 held high: full triangle up+down per 2 * DEMO_SWEEP_HALF_TICKS
// send intervals (~60 s @ 600 half-ticks * 50 ms).
#define DEMO_SWEEP_HALF_TICKS  600
#define DEMO_SPEED_RPM_MAX     2400UL
#define DEMO_ENGINE_RPM_MAX    6500UL
#define PI_HANDSHAKE_BYTE       0xA5
#define PI_HANDSHAKE_TIMEOUT_MS 1000UL

// Odometer: tenths of a mile (uint32 on wire). Wheel model matches ESP32 `el_camino_speed` / Pi defaults.
#ifndef ODO_INITIAL_TENTHS
#define ODO_INITIAL_TENTHS  966486UL  // 96648.5 mi
#endif
#define ODO_WHEEL_CIRC_M    2.1f
#define ODO_SPEED_PPR       1.0f

/** Fuel economy: tank size for DTE and level-derived burn rate (gallons). */
#define FUEL_TANK_GALLONS     20.0f
/** Instant MPG uses fuel level delta over this many samples (~1 s at SEND_INTERVAL 50 ms). */
#define MPG_INST_WINDOW_SAMPLES  20
#define MPG_MIN_MPH           3.0f
#define MPG_MIN_GPH           0.02f
#define MPG_MAX_VALUE         200.0f
/** Refuel: if level rises above trip start by this fraction, reset trip baseline. */
#define MPG_REFUEL_FRAC_DELTA 0.03f
#define FRAM_I2C_ADDR       0x50
#define ODO_FRAM_HDR0       0xE1
#define ODO_FRAM_HDR1       0xD0
/** Legacy 8-byte blob: tenths only. */
#define ODO_FRAM_VER_LEGACY 0x01
/** 12-byte blob: tenths + fractional miles (nanoseconds-of-mile as uint32) so sub-0.1 mi survives reboot. */
#define ODO_FRAM_VER_FRAC   0x02

#define HALL_PULSES_PER_REV   1
#define COIL_PULSES_PER_REV_DEFAULT 8

#define PULSE_TIMEOUT_US  2000000UL

// Fault Detection Thresholds
// With a pull-down resistor, a healthy sensor should never read below
// ADC_LOW_THRESH or above ADC_HIGH_THRESH.
#define ADC_LOW_THRESH      10    // below this = open/short to GND
#define ADC_HIGH_THRESH     1015  // above this = short to VCC
#define FAULT_COUNT_THRESH  10    // 500 ms at 20 Hz
#define HALL_MISSING_THRESH 100   // 5 seconds at 20 Hz
#define COIL_MISSING_THRESH 100

// A0 mode:
// 1 = 3-wire potentiometer (5V/GND/wiper), no pull-down required
// 0 = 2-wire sender + external pull-down on A0
#define POT_IS_3WIRE 1

// Fuel sender (A0) smoothing. EMA reduces ADC/tank slosh noise; do not deadband the value
// sent on the wire — a non-zero deadband quantizes the stream into visible steps on the Pi
// gauge (dashboard applies its own smoothing_alpha). Higher = slower EMA toward raw.
#define POT_FILTER_ALPHA_DIV  32   // higher = slower response
#define POT_OUTPUT_DEADBAND   0    // 0 = continuous EMA out; was 2 (stepped serial ~2 ADC counts)

// Fault Bit Definitions
#define FAULT_POT_OPEN      (1 << 0)   // A0 below threshold (2-wire pull-down mode)
#define FAULT_POT_SHORT     (1 << 1)   // A0 above threshold (2-wire pull-down mode)
#define FAULT_TEMP_OPEN     (1 << 2)   // A1 below threshold
#define FAULT_TEMP_SHORT    (1 << 3)   // A1 above threshold
#define FAULT_PRESS_OPEN    (1 << 4)   // A2 below threshold
#define FAULT_PRESS_SHORT   (1 << 5)   // A2 above threshold
#define FAULT_VOLT_OPEN     (1 << 6)   // synthetic volt channel (unused / reserved)
#define FAULT_VOLT_HIGH     (1 << 7)   // synthetic volt channel (unused / reserved)
#define FAULT_HALL_MISSING  (1 << 8)   // no speed pulses while engine running
#define FAULT_COIL_MISSING  (1 << 9)   // no coil pulses while ignition on
#define FAULT_ALT_STUCK     (1 << 10)  // alternator L never changed state
#define FAULT_POT_POWER     (1 << 11)  // A0 sensor-side fuse monitor low
#define FAULT_TEMP_POWER    (1 << 12)  // A1 sensor-side fuse monitor low
#define FAULT_PRESS_POWER   (1 << 13)  // A2 sensor-side fuse monitor low
#define FAULT_VOLT_POWER    (1 << 14)  // D10 voltage-rail fuse monitor low (optional)

// Voltage Divider Scaling
#define VDIV_R1        27000.0
#define VDIV_R2        10000.0
#define VDIV_RATIO     (VDIV_R2 / (VDIV_R1 + VDIV_R2))
#define AREF_VOLTAGE   5.0

// Thermistor Parameters (tuned for 100k NTC, B3950)
#define THERM_SERIES_R   100000.0
#define THERM_NOMINAL_R  100000.0
#define THERM_NOMINAL_T  25.0
#define THERM_BETA       3950.0
// Temperature calibration offsets (delta C applied after thermistor conversion).
// Room calibration: measured 69F while reported 83F, so offset is -14F.
#define ENGINE_TEMP_CAL_OFFSET_C  0
#define AMBIENT_TEMP_CAL_OFFSET_C 0
// Thermistor ADC polarity: set to 1 when divider wiring is opposite expected polarity.
#define ENGINE_TEMP_INVERT_ADC     1
#define AMBIENT_TEMP_INVERT_ADC    1

// Flag bits
#define FLAG_ALT_OK    0x01
#define FLAG_IGN_ON    0x02

// Forward declaration for Arduino IDE auto-generated prototypes.
struct AnalogFault;

volatile unsigned long hallLastPulseUs = 0;
volatile unsigned long hallPeriodUs    = 0;
volatile unsigned long coilLastPulseUs = 0;
volatile unsigned long coilPeriodUs    = 0;

void hallISR() {
  unsigned long now = micros();
  unsigned long elapsed = now - hallLastPulseUs;
  if (elapsed > 500) {
    hallPeriodUs    = elapsed;
    hallLastPulseUs = now;
  }
}

void coilISR() {
  unsigned long now = micros();
  unsigned long elapsed = now - coilLastPulseUs;
  if (elapsed > 200) {
    coilPeriodUs    = elapsed;
    coilLastPulseUs = now;
  }
}

struct AnalogFault {
  uint8_t lowCount;
  uint8_t highCount;
  uint8_t powerBadCount;
  bool faultLow;
  bool faultHigh;
  bool powerOk;
};

AnalogFault potFault   = {0, 0, 0, false, false, true};
AnalogFault tempFault  = {0, 0, 0, false, false, true};
AnalogFault pressFault = {0, 0, 0, false, false, true};
AnalogFault voltFault  = {0, 0, 0, false, false, true};

uint16_t hallMissingCount = 0;
uint16_t coilMissingCount = 0;
unsigned long lastPiHandshakeMs = 0;

bool altEverHigh = false;
bool altEverLow = false;
uint16_t altStuckCount = 0;
#define ALT_STUCK_THRESH 600  // 30 seconds at 20 Hz

bool potFilterInit = false;
uint32_t potFilteredQ8 = 0;    // fixed-point (raw ADC << 8)
uint16_t potOutputRaw = 0;
uint8_t coilPulsesPerRev = COIL_PULSES_PER_REV_DEFAULT;

uint8_t readCoilPulsesPerRevFromPins() {
  // Selector uses pull-ups: grounded pin reads LOW.
  bool selALow = (digitalRead(PIN_RPM_SEL_A) == LOW);
  bool selBLow = (digitalRead(PIN_RPM_SEL_B) == LOW);

  if (selALow && !selBLow) return 4;   // A low, B high
  if (!selALow && selBLow) return 6;   // A high, B low
  if (!selALow && !selBLow) return 8;  // A high, B high

  // Both grounded is not a valid requested combo; fall back to default.
  return COIL_PULSES_PER_REV_DEFAULT;
}

void checkAnalogFault(AnalogFault &f, uint16_t raw, bool powerPinHigh) {
  // Power monitor debounce: LOW means sensor-side fuse likely tripped.
  if (!powerPinHigh) {
    if (f.powerBadCount < 255) f.powerBadCount++;
  } else {
    f.powerBadCount = 0;
  }
  f.powerOk = (f.powerBadCount < FAULT_COUNT_THRESH);

  // If power is bad, do not classify signal-wire faults.
  if (!f.powerOk) {
    f.lowCount = 0;
    f.highCount = 0;
    f.faultLow = false;
    f.faultHigh = false;
    return;
  }

  if (raw < ADC_LOW_THRESH) {
    if (f.lowCount < 255) f.lowCount++;
  } else {
    f.lowCount = 0;
  }
  f.faultLow = (f.lowCount >= FAULT_COUNT_THRESH);

  if (raw > ADC_HIGH_THRESH) {
    if (f.highCount < 255) f.highCount++;
  } else {
    f.highCount = 0;
  }
  f.faultHigh = (f.highCount >= FAULT_COUNT_THRESH);
}

void checkPotFault(uint16_t raw, bool powerPinHigh) {
  // Keep power monitor handling for both modes.
  if (!powerPinHigh) {
    if (potFault.powerBadCount < 255) potFault.powerBadCount++;
  } else {
    potFault.powerBadCount = 0;
  }
  potFault.powerOk = (potFault.powerBadCount < FAULT_COUNT_THRESH);

#if POT_IS_3WIRE
  // In 3-wire pot mode, endpoints near 0/1023 are valid and should not trip
  // open/short faults based on pull-down assumptions.
  (void)raw;
  potFault.lowCount = 0;
  potFault.highCount = 0;
  potFault.faultLow = false;
  potFault.faultHigh = false;
#else
  // 2-wire sender mode uses pull-down-based window fault checks.
  if (!potFault.powerOk) {
    potFault.lowCount = 0;
    potFault.highCount = 0;
    potFault.faultLow = false;
    potFault.faultHigh = false;
    return;
  }

  if (raw < ADC_LOW_THRESH) {
    if (potFault.lowCount < 255) potFault.lowCount++;
  } else {
    potFault.lowCount = 0;
  }
  potFault.faultLow = (potFault.lowCount >= FAULT_COUNT_THRESH);

  if (raw > ADC_HIGH_THRESH) {
    if (potFault.highCount < 255) potFault.highCount++;
  } else {
    potFault.highCount = 0;
  }
  potFault.faultHigh = (potFault.highCount >= FAULT_COUNT_THRESH);
#endif
}

uint16_t filterPotRaw(uint16_t raw) {
  if (!potFilterInit) {
    potFilteredQ8 = ((uint32_t)raw) << 8;
    potOutputRaw = raw;
    potFilterInit = true;
    return raw;
  }

  uint32_t targetQ8 = ((uint32_t)raw) << 8;
  int32_t deltaQ8 = (int32_t)targetQ8 - (int32_t)potFilteredQ8;
  potFilteredQ8 += (uint32_t)(deltaQ8 / POT_FILTER_ALPHA_DIV);

  uint16_t emaRaw = (uint16_t)((potFilteredQ8 + 128UL) >> 8);
  int16_t diff = (int16_t)emaRaw - (int16_t)potOutputRaw;
  if (diff < 0) diff = -diff;
  if (diff > POT_OUTPUT_DEADBAND) {
    potOutputRaw = emaRaw;
  }
  return potOutputRaw;
}

uint16_t buildFaultWord(bool ignOn, uint32_t engineRpm, uint32_t speedRpm) {
  uint16_t faults = 0;

  if (potFault.faultLow) faults |= FAULT_POT_OPEN;
  if (potFault.faultHigh) faults |= FAULT_POT_SHORT;
  if (tempFault.faultLow) faults |= FAULT_TEMP_OPEN;
  if (tempFault.faultHigh) faults |= FAULT_TEMP_SHORT;
  if (pressFault.faultLow) faults |= FAULT_PRESS_OPEN;
  if (pressFault.faultHigh) faults |= FAULT_PRESS_SHORT;
  if (voltFault.faultLow) faults |= FAULT_VOLT_OPEN;
  if (voltFault.faultHigh) faults |= FAULT_VOLT_HIGH;
  if (!potFault.powerOk) faults |= FAULT_POT_POWER;
  if (!tempFault.powerOk) faults |= FAULT_TEMP_POWER;
  if (!pressFault.powerOk) faults |= FAULT_PRESS_POWER;
  if (!voltFault.powerOk) faults |= FAULT_VOLT_POWER;

  if (ignOn && engineRpm > 500 && speedRpm == 0) {
    if (hallMissingCount < 0xFFFF) hallMissingCount++;
  } else {
    hallMissingCount = 0;
  }
  if (hallMissingCount >= HALL_MISSING_THRESH) faults |= FAULT_HALL_MISSING;

  if (ignOn && engineRpm == 0) {
    if (coilMissingCount < 0xFFFF) coilMissingCount++;
  } else {
    coilMissingCount = 0;
  }
  if (coilMissingCount >= COIL_MISSING_THRESH) faults |= FAULT_COIL_MISSING;

  if (ignOn && engineRpm > 500) {
    if (!altEverHigh || !altEverLow) {
      if (altStuckCount < 0xFFFF) altStuckCount++;
    }
    if (altStuckCount >= ALT_STUCK_THRESH && (!altEverHigh || !altEverLow)) {
      faults |= FAULT_ALT_STUCK;
    }
  }

  return faults;
}

static const uint8_t SYNC[] = { 0xAA, 0x55 };

/** Running odometer: miles×10 (uint32 on wire). Initialized only by framLoadOdometer() in setup(). */
static uint32_t g_odo_tenths = 0;
static double g_odo_frac_miles = 0.0;
/** FRAM load result for UART: deferred until ESP32 link. Two short lines (≤14 chars) so old ESP parsers with
 *  16-byte text caps still show full text; tenths on line 2. */
static char s_fram_read_l1[20];
static char s_fram_read_l2[20];
static bool s_fram_read_uart_emitted = false;

static void setFramReadUartPair(const char *head, uint32_t tenths) {
  strncpy(s_fram_read_l1, head, sizeof(s_fram_read_l1) - 1);
  s_fram_read_l1[sizeof(s_fram_read_l1) - 1] = '\0';
  snprintf(s_fram_read_l2, sizeof(s_fram_read_l2), "t=%lu", (unsigned long)tenths);
}

static void setFramReadUartOne(const char *only) {
  strncpy(s_fram_read_l1, only, sizeof(s_fram_read_l1) - 1);
  s_fram_read_l1[sizeof(s_fram_read_l1) - 1] = '\0';
  s_fram_read_l2[0] = '\0';
}

static void setFramReadUartTwo(const char *a, const char *b) {
  strncpy(s_fram_read_l1, a, sizeof(s_fram_read_l1) - 1);
  s_fram_read_l1[sizeof(s_fram_read_l1) - 1] = '\0';
  strncpy(s_fram_read_l2, b, sizeof(s_fram_read_l2) - 1);
  s_fram_read_l2[sizeof(s_fram_read_l2) - 1] = '\0';
}

/** Emit FRAM read lines once the cluster UART is likely receiving (0xA5 handshake alive) or 6s bench timeout. */
static void tickDeferredFramReadUart() {
  if (s_fram_read_uart_emitted || s_fram_read_l1[0] == '\0') {
    return;
  }
  uint32_t ms = millis();
  if (ms < 200UL) {
    return;
  }
  const bool link_ok =
      (lastPiHandshakeMs != 0) && (ms - lastPiHandshakeMs <= PI_HANDSHAKE_TIMEOUT_MS);
  if (link_ok || ms >= 6000UL) {
    Serial.println(s_fram_read_l1);
    if (s_fram_read_l2[0] != '\0') {
      Serial.println(s_fram_read_l2);
    }
    Serial.flush();
    s_fram_read_uart_emitted = true;
  }
}
#if SENSORPACK_ODOMETER_FRAM
/** Throttle FRAM writes for fractional miles (tenth crossings stamp immediately). */
static uint32_t s_last_odo_fram_ms = 0;
/** UART log throttle for fractional FRAM writes (center screen Sensor log). */
static uint32_t s_last_fram_frac_uart_ms = 0;
#endif

/** Why `framStampOdometer` ran — controls optional `ODO_FRAM write*` UART lines. */
enum : uint8_t {
  FRAM_STAMP_SILENT = 0,
  FRAM_STAMP_FRAC = 1,
  FRAM_STAMP_TENTH = 2,
  FRAM_STAMP_INIT = 3,
};

#if SENSORPACK_ODOMETER_FRAM

static bool framWriteBytes(uint16_t memAddr, const uint8_t *buf, uint8_t len) {
  Wire.beginTransmission((uint8_t)FRAM_I2C_ADDR);
  Wire.write((uint8_t)(memAddr >> 8));
  Wire.write((uint8_t)(memAddr & 0xFF));
  for (uint8_t i = 0; i < len; i++) {
    Wire.write(buf[i]);
  }
  return Wire.endTransmission() == 0;
}

static bool framReadBytes(uint16_t memAddr, uint8_t *buf, uint8_t len) {
  while (Wire.available() > 0) {
    (void)Wire.read();
  }
  Wire.beginTransmission((uint8_t)FRAM_I2C_ADDR);
  Wire.write((uint8_t)(memAddr >> 8));
  Wire.write((uint8_t)(memAddr & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  int got = Wire.requestFrom((int)FRAM_I2C_ADDR, (int)len);
  if (got != (int)len) {
    return false;
  }
  for (uint8_t i = 0; i < len; i++) {
    int v = Wire.read();
    if (v < 0) {
      return false;
    }
    buf[i] = (uint8_t)v;
  }
  return true;
}

#endif

// Snapshot g_odo_tenths + g_odo_frac_miles to FRAM (v2: 12 bytes). Retries on failed write only.
static void framStampOdometer(uint8_t why) {
#if SENSORPACK_ODOMETER_FRAM
  uint8_t b[12];
  b[0] = ODO_FRAM_HDR0;
  b[1] = ODO_FRAM_HDR1;
  b[2] = ODO_FRAM_VER_FRAC;
  b[3] = 0;
  b[4] = (uint8_t)(g_odo_tenths >> 24);
  b[5] = (uint8_t)(g_odo_tenths >> 16);
  b[6] = (uint8_t)(g_odo_tenths >> 8);
  b[7] = (uint8_t)g_odo_tenths;
  double frac = g_odo_frac_miles;
  if (frac < 0.0) {
    frac = 0.0;
  }
  if (frac >= 0.1) {
    frac = 0.099999999;
  }
  uint32_t fn = (uint32_t)(frac * 1.0e9);
  if (fn > 99999999UL) {
    fn = 99999999UL;
  }
  b[8] = (uint8_t)(fn >> 24);
  b[9] = (uint8_t)(fn >> 16);
  b[10] = (uint8_t)(fn >> 8);
  b[11] = (uint8_t)fn;
  bool ok = false;
  for (uint8_t attempt = 0; attempt < 5; attempt++) {
    if (framWriteBytes(0, b, 12)) {
      ok = true;
      break;
    }
    delay(2);
  }
  if (!ok) {
    Serial.println(F("FRAM wr ERR"));
    return;
  }
  if (why == FRAM_STAMP_SILENT) {
    return;
  }
  if (why == FRAM_STAMP_TENTH) {
    Serial.println(F("FRAM wr 10th"));
    return;
  }
  if (why == FRAM_STAMP_INIT) {
    Serial.println(F("FRAM wr init"));
    return;
  }
  if (why == FRAM_STAMP_FRAC) {
    uint32_t ms = millis();
    if (ms - s_last_fram_frac_uart_ms >= 5000UL) {
      s_last_fram_frac_uart_ms = ms;
      Serial.println(F("FRAM wr frac"));
    }
  }
#else
  (void)why;
#endif
}

/** UART banner after FRAM load: 5-line animation + 6-digit odometer (tenths mi), 500 ms between lines. */
static void emitOdometerBootBanner() {
  static const char anim[5][7] = {
      "------", "--<>--", "-<##>-", "<####>", "######",
  };
  for (uint8_t i = 0; i < 5; i++) {
    Serial.println(anim[i]);
    Serial.flush();
    delay(500);
  }
  char buf[8];
  uint32_t o = g_odo_tenths;
  if (o > 999999UL) {
    o = 999999UL;
  }
  snprintf(buf, sizeof(buf), "%06lu", (unsigned long)o);
  Serial.println(buf);
  Serial.flush();
}

static void framLoadOdometer() {
#if SENSORPACK_ODOMETER_FRAM
  uint8_t b[12];
  for (uint8_t boot_try = 0; boot_try < 25; boot_try++) {
    if (!framReadBytes(0, b, 12)) {
      delay(10);
      continue;
    }
    if (b[0] != ODO_FRAM_HDR0 || b[1] != ODO_FRAM_HDR1) {
      delay(10);
      continue;
    }
    uint32_t tenths =
        ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | (uint32_t)b[7];
    if (b[2] == ODO_FRAM_VER_FRAC) {
      g_odo_tenths = tenths;
      uint32_t fn =
          ((uint32_t)b[8] << 24) | ((uint32_t)b[9] << 16) | ((uint32_t)b[10] << 8) | (uint32_t)b[11];
      g_odo_frac_miles = (double)fn / 1.0e9;
      while (g_odo_frac_miles >= 0.1 && g_odo_tenths < 99999990UL) {
        g_odo_frac_miles -= 0.1;
        g_odo_tenths++;
      }
      setFramReadUartPair("FRAM rd v2 OK", g_odo_tenths);
      return;
    }
    if (b[2] == ODO_FRAM_VER_LEGACY) {
      g_odo_tenths = tenths;
      g_odo_frac_miles = 0.0;
      setFramReadUartPair("FRAM rd v1 OK", g_odo_tenths);
      return;
    }
    // Header OK but unknown version: accept tenths only if in range, then upgrade blob on disk.
    if (tenths <= 99999990UL) {
      g_odo_tenths = tenths;
      g_odo_frac_miles = 0.0;
      setFramReadUartPair("FRAM rd fix OK", g_odo_tenths);
      framStampOdometer(FRAM_STAMP_INIT);
      return;
    }
    delay(10);
  }
  // Legacy 8-byte image: read again as 8 bytes (older firmware only wrote 8).
  uint8_t b8[8];
  for (uint8_t boot_try = 0; boot_try < 4; boot_try++) {
    if (!framReadBytes(0, b8, 8)) {
      delay(5);
      continue;
    }
    if (b8[0] == ODO_FRAM_HDR0 && b8[1] == ODO_FRAM_HDR1 && b8[2] == ODO_FRAM_VER_LEGACY) {
      g_odo_tenths =
          ((uint32_t)b8[4] << 24) | ((uint32_t)b8[5] << 16) | ((uint32_t)b8[6] << 8) | (uint32_t)b8[7];
      g_odo_frac_miles = 0.0;
      setFramReadUartPair("FRAM rd L8 OK", g_odo_tenths);
      framStampOdometer(FRAM_STAMP_INIT);
      return;
    }
    delay(5);
  }
  g_odo_tenths = ODO_INITIAL_TENTHS;
  g_odo_frac_miles = 0.0;
  setFramReadUartTwo("FRAM rd FAIL", "factory");
  framStampOdometer(FRAM_STAMP_INIT);
#else
  g_odo_tenths = ODO_INITIAL_TENTHS;
  g_odo_frac_miles = 0.0;
  setFramReadUartOne("FRAM HW off");
#endif
}

static float speedRpmToMph(uint32_t speedRpm) {
  if (speedRpm == 0) {
    return 0.0f;
  }
  float wheel_rps = (((float)speedRpm) / ODO_SPEED_PPR) / 60.0f;
  float speed_mps = wheel_rps * ODO_WHEEL_CIRC_M;
  return speed_mps * 2.236936f;
}

static void odometerAccumulateFromSpeed(uint32_t speedRpm) {
  float mph = speedRpmToMph(speedRpm);
  if (mph <= 0.05f) {
    return;
  }
  const double dt_h = (double)SEND_INTERVAL / 3600000.0;
  g_odo_frac_miles += (double)mph * dt_h;
  // Miles since last whole tenth = g_odo_frac_miles; crossing 0.1 bumps odometer and stamps FRAM.
  while (g_odo_frac_miles >= 0.1 && g_odo_tenths < 99999990UL) {
    g_odo_frac_miles -= 0.1;
    g_odo_tenths++;
    framStampOdometer(FRAM_STAMP_TENTH);
  }
#if SENSORPACK_ODOMETER_FRAM
  // Persist sub-0.1 mi progress so a reboot does not lose hall-faked distance before the next tenth.
  {
    uint32_t ms = millis();
    if (g_odo_frac_miles > 1e-9 && (ms - s_last_odo_fram_ms >= 2000UL)) {
      s_last_odo_fram_ms = ms;
      framStampOdometer(FRAM_STAMP_FRAC);
    }
  }
#endif
}

static float s_mpg_inst_ring[MPG_INST_WINDOW_SAMPLES];
static uint8_t s_mpg_inst_ring_i = 0;
static uint8_t s_mpg_inst_ring_count = 0;
static uint32_t s_trip_odo_tenths0 = 0;
static double s_trip_odo_frac0 = 0.0;
static float s_trip_fuel_frac0 = 0.0f;
static bool s_prev_ign = false;

static float adcToFuelFrac(uint16_t adc) {
  float t = (float)adc / 1023.0f;
  if (t < 0.0f) {
    t = 0.0f;
  }
  if (t > 1.0f) {
    t = 1.0f;
  }
  return t;
}

/**
 * Instant MPG from fuel level drop vs 20 gal tank over MPG_INST_WINDOW_SAMPLES;
 * trip average since key-on from odometer delta vs fuel used; DTE = gal_remaining * mpg.
 */
static void fuelEconomyUpdate(uint16_t potRaw, uint32_t speedRpm, bool ignOn, uint16_t *mpg_inst_x10,
                              uint16_t *mpg_avg_x10, uint32_t *dte_tenths) {
  *mpg_inst_x10 = 0;
  *mpg_avg_x10 = 0;
  *dte_tenths = 0;

  float fuel_frac = adcToFuelFrac(potRaw);
  float mph = speedRpmToMph(speedRpm);

  s_mpg_inst_ring[s_mpg_inst_ring_i] = fuel_frac;
  s_mpg_inst_ring_i = (uint8_t)((s_mpg_inst_ring_i + 1) % MPG_INST_WINDOW_SAMPLES);
  if (s_mpg_inst_ring_count < MPG_INST_WINDOW_SAMPLES) {
    s_mpg_inst_ring_count++;
  }

  if (ignOn && !s_prev_ign) {
    s_trip_odo_tenths0 = g_odo_tenths;
    s_trip_odo_frac0 = g_odo_frac_miles;
    s_trip_fuel_frac0 = fuel_frac;
  }
  s_prev_ign = ignOn;

  float mpg_inst = 0.0f;
  if (s_mpg_inst_ring_count >= MPG_INST_WINDOW_SAMPLES && ignOn) {
    float f_old = s_mpg_inst_ring[s_mpg_inst_ring_i];
    float delta_f = f_old - fuel_frac;
    float window_s = (float)MPG_INST_WINDOW_SAMPLES * (float)SEND_INTERVAL / 1000.0f;
    if (delta_f > 0.0f && window_s > 0.0f) {
      float gph = (delta_f * FUEL_TANK_GALLONS) / (window_s / 3600.0f);
      if (gph >= MPG_MIN_GPH && mph >= MPG_MIN_MPH) {
        mpg_inst = mph / gph;
      }
    }
  }
  if (mpg_inst > MPG_MAX_VALUE) {
    mpg_inst = MPG_MAX_VALUE;
  }
  if (mpg_inst > 0.5f && mpg_inst < 500.0f) {
    *mpg_inst_x10 = (uint16_t)(mpg_inst * 10.0f + 0.5f);
  }

  if (ignOn && fuel_frac > s_trip_fuel_frac0 + MPG_REFUEL_FRAC_DELTA) {
    s_trip_odo_tenths0 = g_odo_tenths;
    s_trip_odo_frac0 = g_odo_frac_miles;
    s_trip_fuel_frac0 = fuel_frac;
  }

  float avg_mpg = 0.0f;
  if (ignOn) {
    double trip_miles = 0.0;
    if (g_odo_tenths >= s_trip_odo_tenths0) {
      trip_miles = (double)(g_odo_tenths - s_trip_odo_tenths0) * 0.1 + (g_odo_frac_miles - s_trip_odo_frac0);
    }
    if (trip_miles < 0.0) {
      trip_miles = 0.0;
    }
    float fuel_used = FUEL_TANK_GALLONS * (s_trip_fuel_frac0 - fuel_frac);
    if (fuel_used < 0.0f) {
      fuel_used = 0.0f;
    }
    if (trip_miles > 0.05f && fuel_used > 0.01f) {
      avg_mpg = (float)(trip_miles / (double)fuel_used);
    }
  }
  if (avg_mpg > MPG_MAX_VALUE) {
    avg_mpg = MPG_MAX_VALUE;
  }
  if (avg_mpg > 0.5f && avg_mpg < 500.0f) {
    *mpg_avg_x10 = (uint16_t)(avg_mpg * 10.0f + 0.5f);
  }

  float gal_rem = FUEL_TANK_GALLONS * fuel_frac;
  float mpg_for_dte = 0.0f;
  if (avg_mpg > 1.0f) {
    mpg_for_dte = avg_mpg;
  } else if (mpg_inst > 1.0f) {
    mpg_for_dte = mpg_inst;
  }
  if (gal_rem > 0.0f && mpg_for_dte > 0.0f) {
    float dte_mi = gal_rem * mpg_for_dte;
    if (dte_mi < 0.0f) {
      dte_mi = 0.0f;
    }
    uint32_t tenths = (uint32_t)(dte_mi * 10.0f + 0.5f);
    if (tenths > 99999990UL) {
      tenths = 99999990UL;
    }
    *dte_tenths = tenths;
  }
}

void updatePiHandshakeStatus() {
  while (Serial.available() > 0) {
    int incoming = Serial.read();
    if (incoming == PI_HANDSHAKE_BYTE) {
      lastPiHandshakeMs = millis();
    }
  }

  bool handshakeAlive = (millis() - lastPiHandshakeMs) <= PI_HANDSHAKE_TIMEOUT_MS;
  digitalWrite(PIN_HANDSHAKE_LED, handshakeAlive ? HIGH : LOW);
}

void sendPacket(uint16_t pot, uint16_t therm, uint16_t pressure,
                uint16_t voltage, uint16_t ambientTherm, uint32_t speedRpm, uint32_t engineRpm,
                uint8_t flags, uint16_t faults, uint32_t odoTenths, uint16_t mpgInstX10,
                uint16_t mpgAvgX10, uint32_t dteTenths) {
  uint8_t payload[33];

  payload[0] = (pot >> 8) & 0xFF;
  payload[1] = pot & 0xFF;
  payload[2] = (therm >> 8) & 0xFF;
  payload[3] = therm & 0xFF;
  payload[4] = (pressure >> 8) & 0xFF;
  payload[5] = pressure & 0xFF;
  payload[6] = (voltage >> 8) & 0xFF;
  payload[7] = voltage & 0xFF;
  payload[8] = (ambientTherm >> 8) & 0xFF;
  payload[9] = ambientTherm & 0xFF;
  payload[10] = (speedRpm >> 24) & 0xFF;
  payload[11] = (speedRpm >> 16) & 0xFF;
  payload[12] = (speedRpm >> 8) & 0xFF;
  payload[13] = speedRpm & 0xFF;
  payload[14] = (engineRpm >> 24) & 0xFF;
  payload[15] = (engineRpm >> 16) & 0xFF;
  payload[16] = (engineRpm >> 8) & 0xFF;
  payload[17] = engineRpm & 0xFF;
  payload[18] = flags;
  payload[19] = (faults >> 8) & 0xFF;
  payload[20] = faults & 0xFF;
  payload[21] = (odoTenths >> 24) & 0xFF;
  payload[22] = (odoTenths >> 16) & 0xFF;
  payload[23] = (odoTenths >> 8) & 0xFF;
  payload[24] = odoTenths & 0xFF;
  payload[25] = (mpgInstX10 >> 8) & 0xFF;
  payload[26] = mpgInstX10 & 0xFF;
  payload[27] = (mpgAvgX10 >> 8) & 0xFF;
  payload[28] = mpgAvgX10 & 0xFF;
  payload[29] = (dteTenths >> 24) & 0xFF;
  payload[30] = (dteTenths >> 16) & 0xFF;
  payload[31] = (dteTenths >> 8) & 0xFF;
  payload[32] = dteTenths & 0xFF;

  uint8_t crc = 0;
  for (uint8_t i = 0; i < sizeof(payload); i++) {
    crc ^= payload[i];
  }

  Serial.write(SYNC, 2);
  Serial.write(payload, sizeof(payload));
  Serial.write(crc);
}

uint32_t calcRpm(unsigned long periodUs, unsigned long lastPulseUs,
                 uint8_t pulsesPerRev) {
  if (periodUs == 0) return 0;
  if ((micros() - lastPulseUs) > PULSE_TIMEOUT_US) return 0;
  return 60000000UL / (periodUs * pulsesPerRev);
}

// 0..1023 triangle vs monotonic tick (period 2 * DEMO_SWEEP_HALF_TICKS steps).
static uint16_t demoTriangleU16(uint32_t tick) {
  const uint32_t half = (uint32_t)DEMO_SWEEP_HALF_TICKS;
  uint32_t mod = tick % (2u * half);
  if (mod < half) {
    return (uint16_t)((mod * 1023UL) / half);
  }
  mod -= half;
  return (uint16_t)(((half - mod) * 1023UL) / half);
}

static uint32_t demoRpmFromTick(uint32_t tick, uint32_t maxRpm) {
  uint16_t u = demoTriangleU16(tick);
  return ((uint32_t)u * maxRpm) / 1023UL;
}

/** Bench dummy data: plausible MPG / DTE tied to sweep tick and demo fuel ADC (20 gal model). */
static void demoEconomySample(uint32_t demoTick, uint16_t potAdc, uint16_t *mpg_inst_x10, uint16_t *mpg_avg_x10,
                               uint32_t *dte_tenths) {
  const uint32_t ph_inst = demoTick + 560u;
  const uint32_t ph_avg = demoTick + 200u;
  uint16_t tri_inst = demoTriangleU16(ph_inst);
  uint16_t tri_avg = demoTriangleU16(ph_avg);
  // Instant ~10..28 MPG, trip avg ~12..22 MPG (different phases so both needles move).
  *mpg_inst_x10 = (uint16_t)(100u + ((uint32_t)tri_inst * 180u) / 1023u);
  *mpg_avg_x10 = (uint16_t)(120u + ((uint32_t)tri_avg * 100u) / 1023u);
  float fuel_frac = (float)potAdc / 1023.0f;
  if (fuel_frac < 0.0f) {
    fuel_frac = 0.0f;
  }
  if (fuel_frac > 1.0f) {
    fuel_frac = 1.0f;
  }
  float gal_rem = fuel_frac * FUEL_TANK_GALLONS;
  float dte_mi = gal_rem * ((float)*mpg_avg_x10 / 10.0f);
  uint32_t tenths = (uint32_t)(dte_mi * 10.0f + 0.5f);
  if (tenths > 99999990UL) {
    tenths = 99999990UL;
  }
  *dte_tenths = tenths;
}

float adcToVoltage(uint16_t raw) {
  return ((float)raw / 1023.0) * AREF_VOLTAGE / VDIV_RATIO;
}

float thermistorTempC(uint16_t raw) {
  if (raw == 0 || raw >= 1023) return -999.0;
  float r = THERM_SERIES_R * ((1023.0 / (float)raw) - 1.0);
  if (r <= 0.0) return -999.0;
  float steinhart = log(r / THERM_NOMINAL_R) / THERM_BETA;
  steinhart += 1.0 / (THERM_NOMINAL_T + 273.15);
  return (1.0 / steinhart) - 273.15;
}

uint16_t thermistorRawFromTempC(float tempC) {
  float tKelvin = tempC + 273.15;
  if (tKelvin <= 0.0) return 1;

  float t0Kelvin = THERM_NOMINAL_T + 273.15;
  float lnRatio = THERM_BETA * ((1.0 / tKelvin) - (1.0 / t0Kelvin));
  float r = THERM_NOMINAL_R * exp(lnRatio);
  float raw = 1023.0 / (1.0 + (r / THERM_SERIES_R));

  if (raw < 1.0) raw = 1.0;
  if (raw > 1022.0) raw = 1022.0;
  return (uint16_t)(raw + 0.5);
}

uint16_t applyThermistorCalOffset(uint16_t raw, float offsetC) {
  if (offsetC > -0.0001 && offsetC < 0.0001) return raw;
  float tempC = thermistorTempC(raw);
  if (tempC < -900.0) return raw;
  return thermistorRawFromTempC(tempC + offsetC);
}

uint16_t maybeInvertAdcRaw(uint16_t raw, bool invert) {
  if (!invert) return raw;
  if (raw > 1023) raw = 1023;
  return (uint16_t)(1023 - raw);
}

// Fills the legacy "voltage" ADC slot for amp / electrical gauges: key-off = discharge band,
// key-on + alternator L high = charge band (matches FLAG_ALT_OK / D4 sense).
static uint16_t synthesizedChargeDischargeAdc(bool ignOn, bool altLOk) {
  if (!ignOn) {
    return 400U;
  }
  if (altLOk) {
    return 720U;
  }
  return 530U;
}

void setup() {
  Serial.begin(SERIAL_BAUD);

  pinMode(PIN_POT, INPUT);
  pinMode(PIN_THERMISTOR, INPUT);
  pinMode(PIN_PRESSURE, INPUT);
  pinMode(PIN_AMBIENT_THERMISTOR, INPUT);
  pinMode(PIN_HALL, INPUT_PULLUP);
  pinMode(PIN_COIL, INPUT_PULLUP);
  pinMode(PIN_ALT_L, INPUT_PULLUP);
  pinMode(PIN_RPM_SEL_A, INPUT_PULLUP);
  pinMode(PIN_IGN_SENSE, INPUT);
  pinMode(PIN_POT_PWR_MON, INPUT);
  pinMode(PIN_TEMP_PWR_MON, INPUT);
  pinMode(PIN_PRESS_PWR_MON, INPUT);
  pinMode(PIN_VOLT_PWR_MON, INPUT);
  pinMode(PIN_FAULT_DISABLE, INPUT);
  pinMode(PIN_RPM_SEL_B, INPUT_PULLUP);
  pinMode(PIN_HANDSHAKE_LED, OUTPUT);
  digitalWrite(PIN_HANDSHAKE_LED, LOW);
  coilPulsesPerRev = readCoilPulsesPerRevFromPins();

#if SENSORPACK_ODOMETER_FRAM
  Wire.begin();
  Wire.setClock(100000);
  delay(100);
#endif
  framLoadOdometer();
#if SENSORPACK_ODOMETER_FRAM
  s_last_odo_fram_ms = millis();
#endif
  emitOdometerBootBanner();

  attachInterrupt(digitalPinToInterrupt(PIN_HALL), hallISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_COIL), coilISR, FALLING);
}

void loop() {
  static unsigned long lastSend = 0;
  unsigned long now = millis();
  updatePiHandshakeStatus();
  tickDeferredFramReadUart();

  if (now - lastSend < SEND_INTERVAL) return;
  lastSend = now;

  bool faultLoggingDisabled = (digitalRead(PIN_FAULT_DISABLE) == HIGH);
  if (faultLoggingDisabled) {
    static uint32_t demoSweepTick = 0;
    demoSweepTick++;

    // Stagger phases so each channel is visibly distinct on the cluster.
    const uint32_t phPot = demoSweepTick;
    const uint32_t phEng = demoSweepTick + 80;
    const uint32_t phTmp = demoSweepTick + 160;
    const uint32_t phAmb = demoSweepTick + 240;
    const uint32_t phPrs = demoSweepTick + 320;
    const uint32_t phVlt = demoSweepTick + 400;
    const uint32_t phSpd = demoSweepTick + 480;

    uint16_t potGaugeRaw = demoTriangleU16(phPot);
    uint16_t thermRawCal = demoTriangleU16(phTmp);
    uint16_t presRaw = demoTriangleU16(phPrs);
    uint16_t voltRaw = demoTriangleU16(phVlt);
    uint16_t ambientThermRawCal = demoTriangleU16(phAmb);
    uint32_t engineRpm = demoRpmFromTick(phEng, DEMO_ENGINE_RPM_MAX);
    uint32_t speedRpm = demoRpmFromTick(phSpd, DEMO_SPEED_RPM_MAX);

    uint16_t demo_mpg_i = 0;
    uint16_t demo_mpg_a = 0;
    uint32_t demo_dte = 0;
    demoEconomySample(demoSweepTick, potGaugeRaw, &demo_mpg_i, &demo_mpg_a, &demo_dte);

    uint8_t flags = FLAG_ALT_OK | FLAG_IGN_ON;
    sendPacket(potGaugeRaw, thermRawCal, presRaw, voltRaw, ambientThermRawCal, speedRpm, engineRpm, flags,
               0, g_odo_tenths, demo_mpg_i, demo_mpg_a, demo_dte);
    return;
  }

  uint32_t potSum = 0;
  uint32_t thermSum = 0;
  uint32_t presSum = 0;
  uint32_t ambientThermSum = 0;
  for (uint8_t i = 0; i < 4; i++) {
    potSum += analogRead(PIN_POT);
    thermSum += analogRead(PIN_THERMISTOR);
    presSum += analogRead(PIN_PRESSURE);
    ambientThermSum += analogRead(PIN_AMBIENT_THERMISTOR);
  }
  uint16_t potRaw = potSum >> 2;
  uint16_t thermRaw = thermSum >> 2;
  uint16_t presRaw = presSum >> 2;
  uint16_t ambientThermRaw = ambientThermSum >> 2;
  uint16_t thermRawPolarity = maybeInvertAdcRaw(thermRaw, ENGINE_TEMP_INVERT_ADC != 0);
  uint16_t ambientThermRawPolarity = maybeInvertAdcRaw(ambientThermRaw, AMBIENT_TEMP_INVERT_ADC != 0);
  uint16_t thermRawCal = applyThermistorCalOffset(thermRawPolarity, ENGINE_TEMP_CAL_OFFSET_C);
  uint16_t ambientThermRawCal = applyThermistorCalOffset(ambientThermRawPolarity, AMBIENT_TEMP_CAL_OFFSET_C);
  uint16_t potGaugeRaw = filterPotRaw(potRaw);

  bool potPowerHigh = (digitalRead(PIN_POT_PWR_MON) == HIGH);
  bool tempPowerHigh = (digitalRead(PIN_TEMP_PWR_MON) == HIGH);
  bool pressPowerHigh = (digitalRead(PIN_PRESS_PWR_MON) == HIGH);
  bool voltPowerHigh = (digitalRead(PIN_VOLT_PWR_MON) == HIGH);

  const bool altState = (digitalRead(PIN_ALT_L) == HIGH);
  const bool ignOn = (digitalRead(PIN_IGN_SENSE) == HIGH);
  const uint16_t voltRaw = synthesizedChargeDischargeAdc(ignOn, altState);

  checkPotFault(potRaw, potPowerHigh);
  checkAnalogFault(tempFault, thermRaw, tempPowerHigh);
  checkAnalogFault(pressFault, presRaw, pressPowerHigh);
  checkAnalogFault(voltFault, voltRaw, voltPowerHigh);

  noInterrupts();
  unsigned long hPeriod = hallPeriodUs;
  unsigned long hLast = hallLastPulseUs;
  unsigned long cPeriod = coilPeriodUs;
  unsigned long cLast = coilLastPulseUs;
  interrupts();

  coilPulsesPerRev = readCoilPulsesPerRevFromPins();
  uint32_t speedRpm = calcRpm(hPeriod, hLast, HALL_PULSES_PER_REV);
  uint32_t engineRpm = calcRpm(cPeriod, cLast, coilPulsesPerRev);

  uint8_t flags = 0;
  if (altState) {
    flags |= FLAG_ALT_OK;
    altEverHigh = true;
  } else {
    altEverLow = true;
  }
  if (ignOn) flags |= FLAG_IGN_ON;

  uint16_t faults = buildFaultWord(ignOn, engineRpm, speedRpm);

  odometerAccumulateFromSpeed(speedRpm);

  uint16_t mpg_ix10 = 0;
  uint16_t mpg_ax10 = 0;
  uint32_t dte_tenths = 0;
  fuelEconomyUpdate(potGaugeRaw, speedRpm, ignOn, &mpg_ix10, &mpg_ax10, &dte_tenths);

  sendPacket(potGaugeRaw, thermRawCal, presRaw, voltRaw, ambientThermRawCal, speedRpm, engineRpm, flags,
             faults, g_odo_tenths, mpg_ix10, mpg_ax10, dte_tenths);
}
