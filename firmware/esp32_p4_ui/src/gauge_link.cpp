#include "gauge_link.h"

#include <el_camino_speed.h>

namespace {
constexpr uint8_t kFlagAltOk = 0x01;
constexpr uint8_t kFlagIgnOn = 0x02;
}  // namespace

void gauge_fill_sensors(GaugeData &d, const SensorLink &link, float wheel_circumference_m) {
  const SensorPacketValues &p = link.last();
  const SensorDerived s = link.derived();
  d.fresh = link.fresh();
  d.mph = d.fresh ? elc_speed_rpm_to_mph(p.speed_rpm, wheel_circumference_m, 1.0f) : 0.0f;
  d.rpm = d.fresh ? (float)p.engine_rpm : 0.0f;
  d.fuel_pct = s.fuel_pct;
  d.oil_psi = s.oil_psi;
  d.coolant_f = d.fresh ? s.coolant_temp_f : 100.0f;
  d.amp = s.system_amp;
  d.charging = !d.fresh || !(p.flags & kFlagIgnOn) ? -1 : ((p.flags & kFlagAltOk) ? 1 : 0);
  float odo = 0.0f;
  d.odo_valid = link.odometerMiles(&odo);
  d.odo_mi = odo;
  // Three-speed automatic: a rough gear from road speed and revs, for display only.
  d.gear = !d.fresh ? 0 : (d.mph < 4.0f ? 1 : ((d.mph > 55.0f || d.rpm > 4200.0f) ? 3 : 2));
  d.econ_valid = d.fresh && p.has_fuel_economy;
  d.mpg_avg = d.econ_valid ? (float)p.mpg_average_x10 * 0.1f : 0.0f;
  d.mpg_inst = d.econ_valid ? (float)p.mpg_instant_x10 * 0.1f : 0.0f;
  d.dte_mi = d.econ_valid ? (float)p.dte_tenths_mile * 0.1f : 0.0f;
}
