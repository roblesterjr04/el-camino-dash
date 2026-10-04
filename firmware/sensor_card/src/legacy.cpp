#include "legacy.h"

void legacy_can_frames(const EngineOut &o, uint8_t seq, ElcCanFrame out[ELC_CAN_SENSOR_FRAMES]) {
  ElcCanSensor s;
  s.pot_adc = o.pot_adc;
  s.therm_adc = o.therm_adc;
  s.pressure_adc = o.pressure_adc;
  s.voltage_adc = o.charge_adc;
  s.ambient_adc = o.ambient_adc;
  s.speed_rpm = o.wheel_rpm;
  s.engine_rpm = o.engine_rpm;
  s.flags = o.flags;
  s.faults = o.faults;
  s.odometer_tenths_mile = o.odo_tenths;
  s.dte_tenths_mile = o.dte_tenths;
  s.mpg_instant_x10 = o.mpg_inst_x10;
  s.mpg_average_x10 = o.mpg_avg_x10;
  s.has_fuel_economy = o.has_econ;
  s.seq = seq;
  elc_can_encode_sensor(s, out);
}

static void be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
static void be32(uint8_t *p, uint32_t v) {
  be16(p, (uint16_t)(v >> 16));
  be16(p + 2, (uint16_t)v);
}

void legacy_uart_frame(const EngineOut &o, uint8_t out[kUartFrameLen]) {
  out[0] = 0xAA;
  out[1] = 0x55;
  uint8_t *p = out + 2;
  be16(p + 0, o.pot_adc);
  be16(p + 2, o.therm_adc);
  be16(p + 4, o.pressure_adc);
  be16(p + 6, o.charge_adc);
  be16(p + 8, o.ambient_adc);
  be32(p + 10, o.wheel_rpm);
  be32(p + 14, o.engine_rpm);
  p[18] = o.flags;
  be16(p + 19, o.faults);
  be32(p + 21, o.odo_tenths);
  be16(p + 25, o.mpg_inst_x10);
  be16(p + 27, o.mpg_avg_x10);
  be32(p + 29, o.dte_tenths);
  uint8_t x = 0;
  for (int i = 0; i < 33; i++) x ^= p[i];
  out[35] = x;
}
