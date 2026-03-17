/*
 * sensor_pack.ino
 *
 * Reads sensors for a classic car gauge cluster with fault detection.
 * Transmits binary packets over hardware serial to a Raspberry Pi.
 *
 * Wiring - each analog sensor gets a pull-down resistor:
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
 *   This ensures:
 *     - Normal operation reads ~15 to ~1008
 *     - Open circuit (wire break) reads < LOW_THRESHOLD
 *     - Short to VCC reads > HIGH_THRESHOLD
 *     - Short to GND reads < LOW_THRESHOLD
 *
 * Sensors:
 *   A0 - Variable resistor (fuel level or similar)
 *   A1 - Thermistor (engine temperature)
 *   A2 - Pressure sensor (oil pressure)
 *   A3 - System voltage (via voltage divider)
 *   A4 - Thermistor (ambient/outside temperature)
 *   D2 - Hall effect sensor (vehicle speed)
 *   D3 - Ignition coil tach signal (engine RPM)
 *   D4 - Alternator "L" terminal (charge indicator)
 *   D6 - Ignition sense (12V switched through voltage divider)
 *   D7 - Pot power monitor (sensor side of PTC fuse)
 *   D8 - Thermistor power monitor (sensor side of PTC fuse)
 *   D9 - Pressure power monitor (sensor side of PTC fuse)
 *   D10 - Voltage sense power monitor (sensor side of PTC fuse)
 *   D11 - Fault logging disable (HIGH = suppress fault reporting)
 *
 * Packet format (binary, big-endian), 22 bytes:
 *   [0xAA] [0x55]          - sync header (2)
 *   [pot_hi] [pot_lo]      - variable resistor raw ADC (2)
 *   [temp_hi] [temp_lo]    - engine temp raw ADC (2)
 *   [pres_hi] [pres_lo]    - oil pressure raw ADC (2)
 *   [volt_hi] [volt_lo]    - system voltage raw ADC (2)
 *   [spd_b3..spd_b0]       - vehicle speed RPM (4)
 *   [rpm_b3..rpm_b0]       - engine RPM (4)
 *   [flags]                - digital states (1)
 *   [faults_hi][faults_lo] - fault bitfield (2)
 *   [crc]                  - XOR checksum (1)
 *                                            ------
 *                              Total:         22 bytes
 */

#include <math.h>

// Pin Assignments
#define PIN_POT        A0
#define PIN_THERMISTOR A1
#define PIN_PRESSURE   A2
#define PIN_VOLTAGE    A3
#define PIN_AMBIENT_THERMISTOR A4
#define PIN_HALL       2
#define PIN_COIL       3
#define PIN_ALT_L      4
#define PIN_IGN_SENSE  6   // ignition on/off (through voltage divider)
#define PIN_POT_PWR_MON   7
#define PIN_TEMP_PWR_MON  8
#define PIN_PRESS_PWR_MON 9
#define PIN_VOLT_PWR_MON  10
#define PIN_FAULT_DISABLE  11
#define PIN_HANDSHAKE_LED  13

// Config
#define SERIAL_BAUD    115200
#define SEND_INTERVAL  50
#define PI_HANDSHAKE_BYTE       0xA5
#define PI_HANDSHAKE_TIMEOUT_MS 1000UL

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

// Fuel sender (A0) smoothing/debounce. This is intentionally conservative
// to suppress tank slosh noise in the gauge display.
#define POT_FILTER_ALPHA_DIV  32   // higher = slower response
#define POT_OUTPUT_DEADBAND   2    // ignore tiny output jitter

// Fault Bit Definitions
#define FAULT_POT_OPEN      (1 << 0)   // A0 below threshold
#define FAULT_POT_SHORT     (1 << 1)   // A0 above threshold
#define FAULT_TEMP_OPEN     (1 << 2)   // A1 below threshold
#define FAULT_TEMP_SHORT    (1 << 3)   // A1 above threshold
#define FAULT_PRESS_OPEN    (1 << 4)   // A2 below threshold
#define FAULT_PRESS_SHORT   (1 << 5)   // A2 above threshold
#define FAULT_VOLT_OPEN     (1 << 6)   // A3 below threshold (wire broken)
#define FAULT_VOLT_HIGH     (1 << 7)   // A3 above threshold (overvoltage/short)
#define FAULT_HALL_MISSING  (1 << 8)   // no speed pulses while engine running
#define FAULT_COIL_MISSING  (1 << 9)   // no coil pulses while ignition on
#define FAULT_ALT_STUCK     (1 << 10)  // alternator L never changed state
#define FAULT_POT_POWER     (1 << 11)  // A0 sensor-side fuse monitor low
#define FAULT_TEMP_POWER    (1 << 12)  // A1 sensor-side fuse monitor low
#define FAULT_PRESS_POWER   (1 << 13)  // A2 sensor-side fuse monitor low
#define FAULT_VOLT_POWER    (1 << 14)  // A3 sensor-side fuse monitor low

// Voltage Divider Scaling
#define VDIV_R1        27000.0
#define VDIV_R2        10000.0
#define VDIV_RATIO     (VDIV_R2 / (VDIV_R1 + VDIV_R2))
#define AREF_VOLTAGE   5.0

// Thermistor Parameters
#define THERM_SERIES_R   10000.0
#define THERM_NOMINAL_R  10000.0
#define THERM_NOMINAL_T  25.0
#define THERM_BETA       3950.0

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
  bool pin2Low = (digitalRead(PIN_HALL) == LOW);
  bool pin3Low = (digitalRead(PIN_COIL) == LOW);

  if (pin2Low && !pin3Low) return 4;   // 2 low, 3 high
  if (!pin2Low && pin3Low) return 6;   // 2 high, 3 low
  if (!pin2Low && !pin3Low) return 8;  // 2 high, 3 high

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
                uint16_t voltage, uint32_t speedRpm, uint32_t engineRpm,
                uint8_t flags, uint16_t faults) {
  uint8_t payload[19];

  payload[0] = (pot >> 8) & 0xFF;
  payload[1] = pot & 0xFF;
  payload[2] = (therm >> 8) & 0xFF;
  payload[3] = therm & 0xFF;
  payload[4] = (pressure >> 8) & 0xFF;
  payload[5] = pressure & 0xFF;
  payload[6] = (voltage >> 8) & 0xFF;
  payload[7] = voltage & 0xFF;
  payload[8] = (speedRpm >> 24) & 0xFF;
  payload[9] = (speedRpm >> 16) & 0xFF;
  payload[10] = (speedRpm >> 8) & 0xFF;
  payload[11] = speedRpm & 0xFF;
  payload[12] = (engineRpm >> 24) & 0xFF;
  payload[13] = (engineRpm >> 16) & 0xFF;
  payload[14] = (engineRpm >> 8) & 0xFF;
  payload[15] = engineRpm & 0xFF;
  payload[16] = flags;
  payload[17] = (faults >> 8) & 0xFF;
  payload[18] = faults & 0xFF;

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

float adcToVoltage(uint16_t raw) {
  return ((float)raw / 1023.0) * AREF_VOLTAGE / VDIV_RATIO;
}

float thermistorTempC(uint16_t raw) {
  if (raw == 0) return -999.0;
  float r = THERM_SERIES_R * ((1023.0 / (float)raw) - 1.0);
  float steinhart = log(r / THERM_NOMINAL_R) / THERM_BETA;
  steinhart += 1.0 / (THERM_NOMINAL_T + 273.15);
  return (1.0 / steinhart) - 273.15;
}

void setup() {
  Serial.begin(SERIAL_BAUD);

  pinMode(PIN_POT, INPUT);
  pinMode(PIN_THERMISTOR, INPUT);
  pinMode(PIN_PRESSURE, INPUT);
  pinMode(PIN_VOLTAGE, INPUT);
  pinMode(PIN_AMBIENT_THERMISTOR, INPUT);
  pinMode(PIN_HALL, INPUT_PULLUP);
  pinMode(PIN_COIL, INPUT_PULLUP);
  pinMode(PIN_ALT_L, INPUT_PULLUP);
  pinMode(PIN_IGN_SENSE, INPUT);
  pinMode(PIN_POT_PWR_MON, INPUT);
  pinMode(PIN_TEMP_PWR_MON, INPUT);
  pinMode(PIN_PRESS_PWR_MON, INPUT);
  pinMode(PIN_VOLT_PWR_MON, INPUT);
  pinMode(PIN_FAULT_DISABLE, INPUT);
  pinMode(PIN_HANDSHAKE_LED, OUTPUT);
  digitalWrite(PIN_HANDSHAKE_LED, LOW);
  coilPulsesPerRev = readCoilPulsesPerRevFromPins();

  attachInterrupt(digitalPinToInterrupt(PIN_HALL), hallISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_COIL), coilISR, FALLING);
}

void loop() {
  static unsigned long lastSend = 0;
  unsigned long now = millis();
  updatePiHandshakeStatus();

  if (now - lastSend < SEND_INTERVAL) return;
  lastSend = now;

  uint32_t potSum = 0;
  uint32_t thermSum = 0;
  uint32_t presSum = 0;
  uint32_t voltSum = 0;
  uint32_t ambientThermSum = 0;
  for (uint8_t i = 0; i < 4; i++) {
    potSum += analogRead(PIN_POT);
    thermSum += analogRead(PIN_THERMISTOR);
    presSum += analogRead(PIN_PRESSURE);
    voltSum += analogRead(PIN_VOLTAGE);
    ambientThermSum += analogRead(PIN_AMBIENT_THERMISTOR);
  }
  uint16_t potRaw = potSum >> 2;
  uint16_t thermRaw = thermSum >> 2;
  uint16_t presRaw = presSum >> 2;
  uint16_t voltRaw = voltSum >> 2;
  uint16_t ambientThermRaw = ambientThermSum >> 2;
  uint16_t potGaugeRaw = filterPotRaw(potRaw);
  (void)ambientThermRaw;  // sampled for ambient/outside thermistor bring-up on A4

  bool potPowerHigh = (digitalRead(PIN_POT_PWR_MON) == HIGH);
  bool tempPowerHigh = (digitalRead(PIN_TEMP_PWR_MON) == HIGH);
  bool pressPowerHigh = (digitalRead(PIN_PRESS_PWR_MON) == HIGH);
  bool voltPowerHigh = (digitalRead(PIN_VOLT_PWR_MON) == HIGH);

  checkAnalogFault(potFault, potRaw, potPowerHigh);
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
  bool altState = (digitalRead(PIN_ALT_L) == HIGH);
  bool ignOn = (digitalRead(PIN_IGN_SENSE) == HIGH);

  if (altState) {
    flags |= FLAG_ALT_OK;
    altEverHigh = true;
  } else {
    altEverLow = true;
  }
  if (ignOn) flags |= FLAG_IGN_ON;

  bool faultLoggingDisabled = (digitalRead(PIN_FAULT_DISABLE) == HIGH);
  uint16_t faults = faultLoggingDisabled ? 0 : buildFaultWord(ignOn, engineRpm, speedRpm);

  sendPacket(potGaugeRaw, thermRaw, presRaw, voltRaw, speedRpm, engineRpm, flags, faults);
}
