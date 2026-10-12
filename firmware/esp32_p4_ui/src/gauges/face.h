#pragma once

#include <elc_fonts.h>
#include <elc_gauges.h>

#include "widgets.h"

namespace gauge {

/**
 * One gauge face. paint() draws the static dial once (into an off-screen canvas, origin at the
 * square's corner); build() creates the live parts on top; update() runs every frame.
 */
class Face {
 public:
  virtual ~Face();
  virtual void paint(Canvas888 &c, const Geo &g) = 0;
  virtual void build(lv_obj_t *root, const Geo &g) = 0;
  virtual void update(const GaugeData &d, float dt) = 0;
  /** Shows NO DATA while the sensor link is stale. */
  virtual bool uses_sensors() const { return true; }

 protected:
  template <class T>
  T *add(lv_obj_t *root) {
    T *t = new T();
    t->attach(root);
    if (n_ < kMax) {
      shapes_[n_++] = t;
    }
    return t;
  }

 private:
  static constexpr int kMax = 32;
  Shape *shapes_[kMax] = {};
  int n_ = 0;
};

Face *make_classic(GaugeFace f, uint8_t variant);
Face *make_modern(GaugeFace f);
Face *make_fallout(GaugeFace f);

// ---------------------------------------------------------------- small formatting helpers

/** "1:05" from seconds. */
void fmt_mmss(char *out, size_t n, float s);
/** Media title / artist with fallbacks when nothing is playing. */
const char *media_title(const GaugeData &d);
/** The radio's signal for the media face's progress bar, 0..1 (0 when not the radio or not known). */
inline float radio_signal(const GaugeData &d) { return d.media_connected && d.media_radio && d.media_signal > 0 ? d.media_signal : 0.0f; }
const char *media_artist(const GaugeData &d);
/** 12-hour clock pieces. */
int hour12(int h24);

}  // namespace gauge
