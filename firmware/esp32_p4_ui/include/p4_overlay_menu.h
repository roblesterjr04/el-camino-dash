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

  void clearItems();

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

  // LVGL "translate on scroll" style: vertical list, rows shift on a circular curve; center row full opacity.
  // `ids` / `labels` must stay valid until menu is closed or reconfigured.
  void setupHomeTranslateList(const int *ids, const char *const *labels, int count, int16_t margin = 72);

  void homeListBeginDrag(int16_t y);

  // While finger is down on the home list: drag to scroll (call each frame with current y).
  void homeListDragTo(int16_t y);

  // Finger lifted after a drag — snap to nearest row (call before draw).
  void homeListSnap();

  void open();

  void close();

  bool isOpen() const { return _open; }

  // After finger-up: item id, or -2 = backdrop/gap dismiss, -3 = ignored (menu closed),
  // -6 = home list drag ended without row activation (caller should redraw only).
  int onRelease(int16_t x, int16_t y, int16_t drag_px = 0) const;

  bool isHomeTranslateList() const { return _home_list; }

  // highlight_item_id: item id to emphasize (e.g. current mode), or < -900 for none.
  void draw(int16_t highlight_item_id = -999);

private:
  void releaseCache();
  void paintMenu(Arduino_GFX *target, int16_t highlight_item_id);
  void paintHomeTranslateList(Arduino_GFX *target, int16_t highlight_item_id);
  int hitHomeTranslateList(int16_t x, int16_t y) const;
  void homeListRowGeometry(int index, int16_t *out_x, int16_t *out_y, int16_t *out_w, int16_t *out_h,
                            int16_t *out_tx) const;

  Arduino_GFX *_gfx = nullptr;
  int16_t _sw = 0;
  int16_t _sh = 0;
  uint16_t _backdrop = 0x0000;
  bool _round_display = false;
  bool _open = false;
  int _n = 0;
  P4OverlayItemSpec _items[P4_OVERLAY_MAX_ITEMS];

  bool _home_list = false;
  int16_t _home_scroll_y = 0;
  int16_t _home_marg = 72;
  int16_t _home_row_h = 52;
  int16_t _home_drag_last_y = 0;

  class Arduino_Canvas *_canvas = nullptr;
  bool _cache_ok = false;
  int16_t _cache_hi = -999;
  int16_t _cache_scroll = -9999;
};
