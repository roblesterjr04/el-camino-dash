#include <demo_drive.h>

#include <cmath>

namespace {

float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

/** Target speed (mph) over a 60 s loop: idle, pull away through the gears, cruise, pass, stop. */
float target_mph(float t) {
  if (t < 4.0f) return 0.0f;
  if (t < 16.0f) return 55.0f * (t - 4.0f) / 12.0f;
  if (t < 36.0f) return 55.0f + 3.0f * sinf((t - 16.0f) * 0.5f);
  if (t < 42.0f) return 55.0f + 15.0f * (t - 36.0f) / 6.0f;
  if (t < 54.0f) return 70.0f * (1.0f - (t - 42.0f) / 12.0f);
  return 0.0f;
}

/** Inverse of the 100k NTC / B3950 conversion in sensor_packet.cpp: temperature °F → ADC count. */
uint16_t therm_adc_for_f(float f) {
  const float t_k = (f - 32.0f) * 5.0f / 9.0f + 273.15f;
  const float r = 100000.0f * expf(3950.0f * (1.0f / t_k - 1.0f / 298.15f));
  return (uint16_t)clampf(1023.0f / (1.0f + r / 100000.0f), 1.0f, 1022.0f);
}

}  // namespace

void DemoDrive::start(uint32_t now_ms, uint32_t odometer_tenths) {
  t0_ms_ = now_ms;
  last_ms_ = now_ms;
  odo_tenths_ = odometer_tenths;
  odo_frac_ = 0.0f;
  mph_ = 0.0f;
  coolant_f_ = 150.0f;
  fuel_pct_ = 64.0f;
  mpg_avg_ = 17.5f;
}

void DemoDrive::frame(uint32_t now_ms, SensorPacketValues &out) {
  const float dt = clampf((float)(now_ms - last_ms_) / 1000.0f, 0.0f, 0.25f);
  last_ms_ = now_ms;
  const float t = fmodf((float)(now_ms - t0_ms_) / 1000.0f, 60.0f);

  mph_ += (target_mph(t) - mph_) * (1.0f - expf(-2.0f * dt));
  if (mph_ < 0.2f) mph_ = 0.0f;
  const int gear = mph_ < 17.0f ? 1 : (mph_ < 38.0f ? 2 : 3);
  const float ratio[] = {0.0f, 118.0f, 64.0f, 38.0f};
  const float accel = target_mph(t) - mph_;
  float rpm = fmaxf(720.0f + fmaxf(accel, 0.0f) * 60.0f, mph_ * ratio[gear]);
  rpm = clampf(rpm, 0.0f, 5800.0f);

  coolant_f_ += (195.0f - coolant_f_) * (1.0f - expf(-0.05f * dt));
  fuel_pct_ = fmaxf(8.0f, fuel_pct_ - dt * 0.02f);
  const float mpg_inst = mph_ > 3.0f ? clampf(26.0f - fmaxf(accel, 0.0f) * 1.5f - (mph_ > 60.0f ? (mph_ - 60.0f) * 0.3f : 0.0f), 6.0f, 32.0f) : 0.0f;
  if (mph_ > 3.0f) mpg_avg_ += (mpg_inst - mpg_avg_) * (1.0f - expf(-0.02f * dt));

  odo_frac_ += mph_ * dt / 3600.0f;
  while (odo_frac_ >= 0.1f) {
    odo_frac_ -= 0.1f;
    odo_tenths_++;
  }

  out = {};
  out.pot_adc = (uint16_t)(fuel_pct_ / 100.0f * 1023.0f);
  out.therm_adc = therm_adc_for_f(coolant_f_);
  out.pressure_adc = (uint16_t)clampf((14.0f + rpm / 95.0f) / 100.0f * 1023.0f, 0.0f, 1023.0f);
  out.voltage_adc = 720;  // charging
  out.ambient_adc = therm_adc_for_f(72.0f);
  out.speed_rpm = (uint32_t)(mph_ / 2.236936f / 2.1f * 60.0f + 0.5f);
  out.engine_rpm = (uint32_t)rpm;
  out.flags = 0x03;  // ALT OK, IGN ON
  out.faults = 0;
  out.odometer_tenths_mile = odo_tenths_;
  out.has_odometer = true;
  out.mpg_instant_x10 = (uint16_t)(mpg_inst * 10.0f);
  out.mpg_average_x10 = (uint16_t)(mpg_avg_ * 10.0f);
  out.dte_tenths_mile = (uint32_t)(fuel_pct_ / 100.0f * 20.0f * mpg_avg_ * 10.0f);
  out.has_fuel_economy = true;
  out.extended = true;
  out.valid = true;
}
