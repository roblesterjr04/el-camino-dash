#pragma once

// The card's menu page (the UI channel, docs/protocols/can_channels.md; the items are those of
// docs/protocols/sensor_calibration.md, "Setting it: the center's menu"): 64 items under Menu -> Accessories ->
// Sensor card. Values are held in canonical units (litres, degC, kPa) and shown in the center's units (PREFS).
// Nothing here touches the hardware: host-tested.

#include <stdint.h>

#include "calib.h"
#include "card_net.h"
#include "elc_chan_msg.h"
#include "engine.h"
#include "odometer.h"

struct ItemDef;  // a row of the menu table (card_ui.cpp)

class CardUi {
 public:
  struct Ctx {
    cal::Calibration *cal = nullptr;
    Engine *engine = nullptr;
    Odometer *odo = nullptr;
    CardNet *net = nullptr;
    const EngineOut *out = nullptr;
    uint32_t uid = 0;
  };
  void begin(const Ctx &c) {
    _c = c;
    _units = 0;
  }

  /** The center's units (SYSTEM PREFS): bit0 metric (litres), bit1 degC, bit2 kPa. */
  void setUnits(uint8_t units);
  uint8_t units() const { return _units; }

  // ---- descriptions: item by table index (0 .. itemCount() - 1)
  uint8_t itemCount() const;
  bool encodeItem(uint8_t index, ElcChWriter &w);
  /** VALUE (or TEXT, for a text item) of the item; false for a group. */
  bool encodeValue(uint8_t index, ElcChWriter &w);
  /** The item has flag bit4 (the center backs its value up). */
  bool isSetting(uint8_t index) const;

  // ---- input
  void onTransfer(const ElcChTransfer &t, uint32_t now);
  /** The GPS card's motion frame (0x223), for the speed calibration. */
  void onMotion(const ElcCanFrame &f, uint32_t now);
  /** Captures, the GPS run, and the values of the page being looked at. Call every pass of loop(). */
  void poll(uint32_t now);

  /** A setting changed (the menu, the bench console): saved soon, factory-defaults flag cleared, values re-sent. */
  void changed();
  /** Back to the defaults the card is delivered with; the center restores its backup onto it. */
  void resetCalibration();
  /** True once after something that should be written to the FRAM. */
  bool takeDirty() {
    const bool d = _dirty;
    _dirty = false;
    return d;
  }
  /** A capture from the bench console (the same 5 s average as the menu's). False if one is running. */
  bool startCapture(uint8_t ch, int16_t canonical_value, uint8_t asker = 0, uint8_t item = 0xFF);
  bool capturing() const { return _cap.active; }
  const char *captureMessage() const { return _cap_msg; }
  bool captureDone(bool *ok);  // true once per finished capture, with its outcome

  void speedText(char *out, size_t cap) const;
  void statusText(char *out, size_t cap) const;
  bool gpsCalActive() const { return _gps.active; }
  bool startGpsCal(uint32_t now);

  // display-unit conversions, shared with the console
  int32_t dispVolume(int32_t l10) const;   // L x10 -> gal x10 or L x10
  int32_t fromVolume(int32_t d) const;
  int32_t dispTemp(int32_t c10) const;     // degC x10 -> degF x10 or degC x10
  int32_t fromTemp(int32_t d) const;
  int32_t dispDelta(int32_t c10) const;    // a temperature difference
  int32_t fromDelta(int32_t d) const;
  int32_t dispPress(int32_t kpa) const;    // kPa -> psi x10 (exponent -1) or kPa (exponent 0)
  int32_t fromPress(int32_t d) const;

 private:
  const ItemDef *def(uint8_t id) const;
  int32_t value(uint8_t id) const;
  void text(uint8_t id, char *out, size_t cap) const;
  uint8_t set(uint8_t id, int32_t v, uint8_t asker, bool *async);
  void rangeOf(uint8_t id, int8_t *exp, int32_t *mn, int32_t *mx, int32_t *step) const;
  void tableText(uint8_t ch, char *out, size_t cap) const;
  bool sendItemValue(uint8_t id);
  void sendResult(uint8_t item, uint8_t result, uint8_t asker);
  void startRefresh(uint8_t group);
  bool setPoint(cal::ChannelCal &k, uint8_t ch, uint8_t slot, int32_t packed);
  void finishCapture(uint32_t now);
  void gpsFinish(bool apply, const char *msg);

  Ctx _c;
  uint8_t _units = 0;
  bool _dirty = false;
  // the page being looked at
  uint8_t _view = 0xFF;        // group being shown (0xFE top, 0xFF none)
  uint32_t _view_ms = 0, _live_ms = 0;
  uint8_t _live_cursor = 0xFF;
  uint8_t _refresh_group = 0xFF;
  int16_t _refresh_cursor = -1;
  bool _resend_settings = false;
  uint8_t _resend_cursor = 0;
  // capture
  struct Capture {
    bool active = false;
    uint8_t ch = 0, item = 0xFF, asker = 0;
    int16_t value = 0;
    uint32_t start = 0, last = 0;
    float sum = 0, ema = 0, mn = 0, mx = 0;
    uint16_t n = 0;
  } _cap;
  char _cap_msg[48] = {0};
  bool _cap_done = false, _cap_ok = false;
  char _table_note[3][40] = {{0}, {0}, {0}};  // per channel (fuel, temp, oil): why the last capture was refused
  // GPS speed calibration
  struct Gps {
    bool active = false;
    uint32_t start = 0, last_frame = 0, pulses0 = 0;
    float miles = 0, speed_min = 0, speed_max = 0;
    uint8_t asker = 0;
    bool have_fix = false;
    uint16_t cm_s = 0;
  } _gps;
  char _speed_note[32] = {0};
};
