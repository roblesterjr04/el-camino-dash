#pragma once

#include <Arduino_GFX_Library.h>
#include <stdint.h>

// Modal overlay for touch UIs: hidden until open(); draw after main content, then flush once.
// Typical use: main draws gauge → if overlay open, draw() → gfx->flush(true).

#ifndef P4_OVERLAY_MAX_ITEMS
#define P4_OVERLAY_MAX_ITEMS 12
#endif

struct P4OverlayItemSpec {
  int id;
  int16_t x, y, w, h;
  const char *label;
};

class P4OverlayMenu {
public:
  void begin(Arduino_GFX *gfx);

  void setScreenSize(int16_t w, int16_t h);

  void setBackdropColor(uint16_t rgb565) { _backdrop = rgb565; }

  void clearItems() { _n = 0; }

  bool addItem(const P4OverlayItemSpec &spec) {
    if (_n >= P4_OVERLAY_MAX_ITEMS) {
      return false;
    }
    _items[_n++] = spec;
    return true;
  }

  // Two large columns with a gap (taps in gap dismiss). Margin inset from screen edges.
  void setupTwoColumn(int id_left, int id_right, const char *left_lbl, const char *right_lbl, int16_t margin = 24,
                      int16_t gap = 20);

  // Four quadrants (e.g. TACH | CLOCK / WIFI | MORE). Gap between cells; backdrop tap still dismisses.
  void setupGrid2x2(int id_tl, int id_tr, int id_bl, int id_br, const char *lbl_tl, const char *lbl_tr,
                    const char *lbl_bl, const char *lbl_br, int16_t margin = 16, int16_t gap = 12);

  void open();

  void close();

  bool isOpen() const { return _open; }

  // After finger-up: item id, or -2 = backdrop/gap dismiss, -3 = ignored (menu closed)
  int onRelease(int16_t x, int16_t y) const;

  // highlight_item_id: item id to emphasize (e.g. current mode), or < -900 for none.
  void draw(int16_t highlight_item_id = -999);

private:
  void releaseCache();
  void paintMenu(Arduino_GFX *target, int16_t highlight_item_id);

  Arduino_GFX *_gfx = nullptr;
  int16_t _sw = 0;
  int16_t _sh = 0;
  uint16_t _backdrop = 0x0000;
  bool _round_display = false;
  bool _open = false;
  int _n = 0;
  P4OverlayItemSpec _items[P4_OVERLAY_MAX_ITEMS];

  class Arduino_Canvas *_canvas = nullptr;
  bool _cache_ok = false;
  int16_t _cache_hi = -999;
};
