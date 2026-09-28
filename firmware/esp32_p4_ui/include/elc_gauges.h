#pragma once

/**
 * Gauge faces for all three displays (speedometer, center, right), drawn with LVGL.
 *
 * Every face is vector: the static dial (scale, numerals, chrome, brushed metal) is painted once in
 * software when the face is shown and kept as an image; needles, arcs, lamps and readouts are live
 * LVGL objects that repaint only what moved. Faces are designed on an 800-unit square and scale to
 * the panel.
 *
 * Loop contract: fill a GaugeData from the sensor link (and clock/media where relevant), then call
 * gauges_update() every frame before p4_screen_frame().
 */

#include <lvgl.h>
#include <stdint.h>

/** Same numbering as the cluster bus theme register (0 Classic, 1 Fallout, 2 Modern). */
enum class GaugeTheme : uint8_t { Classic = 0, Fallout = 1, Modern = 2 };

enum class GaugeFace : uint8_t {
  Speedo,  // left display
  Tach,    // center views
  Clock,
  Media,
  Econ,
  Right,   // right display: fuel / oil / temp / charge (Classic has two layouts, see variant)
};

struct GaugeData {
  /** A sensor frame arrived recently. When false, sensor faces rest at zero and show NO DATA. */
  bool fresh = false;
  float mph = 0;
  float rpm = 0;
  float fuel_pct = 0;     // 0..100
  float coolant_f = 100;  // °F
  float oil_psi = 0;
  float amp = 0;          // -100..100 (charge meter)
  /** 1 charging, 0 not charging (warning), -1 key off / unknown. */
  int8_t charging = -1;
  bool odo_valid = false;
  float odo_mi = 0;
  float trip_mi = 0;
  /** Gear 1..3, 0 = stopped / unknown. */
  int8_t gear = 0;
  bool econ_valid = false;
  float mpg_avg = 0, mpg_inst = 0;
  float dte_mi = 0;
  float tank_gal = 20;

  /** Wall time from GPS / NTP; when false the clock shows time since boot. */
  bool time_valid = false;
  int hour = 0, minute = 0;
  float second = 0;
  const char *date_text = nullptr;   // e.g. "Mon, Sep 28"
  const char *place_text = nullptr;  // street name or time source

  bool media_connected = false;
  const char *media_title = nullptr;
  const char *media_artist = nullptr;
  float media_elapsed = 0, media_duration = 0;

  /** Short text for the speedometer's odometer slot (sensor-pack messages); nullptr = odometer. */
  const char *banner = nullptr;
};

/**
 * Create the gauge layer under `parent` (a full-screen container). The round glass is the square of
 * `size` px at (x, y) — for a 1024×600 panel, the centred 600×600 square.
 */
void gauges_begin(lv_obj_t *parent, int x, int y, int size);

/** Switch face / theme. The dial is painted (once, then cached) when a face is first shown. variant:
 *  right display Classic layout (0 = four gauges, 1 = fuel with warning lamps). */
void gauges_show(GaugeTheme theme, GaugeFace face, uint8_t variant = 0);

/** Feed the latest values; animates needles toward them. Call every frame. */
void gauges_update(const GaugeData &d, uint32_t now_ms);

/** Hide the whole gauge layer (e.g. behind a full-screen page) without losing the face. */
void gauges_set_hidden(bool hidden);

GaugeTheme gauges_theme();
GaugeFace gauges_face();

/** Milliseconds the last dial paint took (shown by the `perf` console commands). */
uint32_t gauges_last_paint_ms();
