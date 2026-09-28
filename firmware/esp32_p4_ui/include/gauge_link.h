#pragma once

#include <elc_gauges.h>
#include <sensor_link.h>

/**
 * Fill the sensor fields of GaugeData from the sensor link: speed (from wheel RPM), engine RPM,
 * fuel / oil / coolant / charge, odometer and fuel economy. Clock and media fields are left alone.
 */
void gauge_fill_sensors(GaugeData &d, const SensorLink &link, float wheel_circumference_m);
