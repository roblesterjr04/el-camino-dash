#include <sensor_link.h>

void SensorLink::begin(HardwareSerial &ser, int rx_pin, int tx_pin) {
  _ser = &ser;
  ser.setRxBufferSize(2048);
  ser.begin(115200, SERIAL_8N1, rx_pin, tx_pin);
  _parser.begin(ser);
}

bool SensorLink::poll() {
  if (!_ser) {
    return false;
  }
  bool got = false;
  SensorPacketValues v{};
  const uint32_t now = millis();
  // Drain everything buffered; keep the newest frame.
  while (_parser.poll(v, false)) {
    if (!_demo) {
      _held = v;
      got = true;
      _fps_count++;
      _uart_ms = now;
      _have_uart = true;
      _from_bus = false;
    }
  }
  const bool uart_fresh = _have_uart && now - _uart_ms <= SENSOR_PACKET_STALE_MS;
  if (_bus_pending && !_demo && !uart_fresh) {
    _held = _bus;
    got = true;
    _fps_count++;
    _from_bus = true;
  }
  _bus_pending = false;
  if (_demo && (int32_t)(now - _demo_next_ms) >= 0) {
    _demo_next_ms = now + 50;
    _drive.frame(now, _held);
    got = true;
    _fps_count++;
  }
  if (got) {
    _last_ms = now;
    _have = true;
  }
  if (now - _fps_t0 >= 1000) {
    _fps = _fps_count;
    _fps_count = 0;
    _fps_t0 = now;
  }
  if (_keepalive && now - _last_keepalive_ms >= SENSOR_LINK_KEEPALIVE_MS) {
    _ser->write(PI_HANDSHAKE_BYTE);
    _last_keepalive_ms = now;
  }
  return got;
}

void SensorLink::feedBus(const ElcCanSensor &s) {
  SensorPacketValues v{};
  v.pot_adc = s.pot_adc;
  v.therm_adc = s.therm_adc;
  v.pressure_adc = s.pressure_adc;
  v.voltage_adc = s.voltage_adc;
  v.ambient_adc = s.ambient_adc;
  v.speed_rpm = s.speed_rpm;
  v.engine_rpm = s.engine_rpm;
  v.flags = s.flags;
  v.faults = s.faults;
  v.odometer_tenths_mile = s.odometer_tenths_mile;
  v.has_odometer = s.odometer_tenths_mile != 0xFFFFFFFFu;
  v.mpg_instant_x10 = s.mpg_instant_x10;
  v.mpg_average_x10 = s.mpg_average_x10;
  v.dte_tenths_mile = s.dte_tenths_mile;
  v.has_fuel_economy = s.has_fuel_economy;
  v.extended = true;
  v.valid = true;
  _bus = v;
  _bus_pending = true;
}

void SensorLink::setDemo(bool on) {
  if (on == _demo) {
    return;
  }
  _demo = on;
  if (on) {
    // Continue from the real odometer if we have one, so the numbers look plausible.
    _drive.start(millis(), (_have && _held.has_odometer) ? _held.odometer_tenths_mile : 966486u);
    _demo_next_ms = millis();
  } else {
    _have = false;  // show NO DATA until the next real frame instead of the last demo values
  }
}

bool SensorLink::fresh() const { return _have && (millis() - _last_ms) <= SENSOR_PACKET_STALE_MS; }

SensorDerived SensorLink::derived() const {
  if (fresh()) {
    return SensorPacketParser::derive(_held);
  }
  SensorDerived d{};
  d.valid = false;
  d.fuel_pct = 0.0f;
  d.oil_psi = 0.0f;
  d.coolant_temp_f = 100.0f;
  d.system_amp = 0.0f;
  return d;
}

bool SensorLink::odometerMiles(float *out) const {
  if (!fresh() || !_held.has_odometer) {
    return false;
  }
  if (out) {
    *out = (float)_held.odometer_tenths_mile * 0.1f;
  }
  return true;
}
