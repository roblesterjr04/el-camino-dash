#include "p4_overlay_menu.h"

#include <Arduino.h>
#include <Arduino_DataBus.h>
#include <canvas/Arduino_Canvas.h>
#include <cmath>
#include <cstring>

#include "FreeSansBold12pt7b.h"
#include "p4_round_mask.h"

/*
 * Home list “translate on scroll” matches LVGL lv_example_scroll_6.c scroll_event_cb:
 * https://github.com/lvgl/lvgl/blob/e19410f8f8a256609da72cff549598e0df6fa4cf/examples/scroll/lv_example_scroll_6.c
 * - r = container_height * 7 / 10
 * - diff_y = abs(child_y_center - cont_y_center)
 * - x = (diff_y >= r) ? r : (r - sqrt(r*r - diff_y*diff_y))
 * - translate_x = x; opa = LV_OPA_COVER - lv_map(x, 0, r, LV_OPA_TRANSP, LV_OPA_COVER)
 * - Children stay full width (lv_pct(100)); only translate_x shifts them — do not shrink width by x.
 */

// Pi menu_overlay.py _paint_overlay (round): shade (14,10,4,195), panel (40,30,12,225), border (188,148,54,235)

/** Extra scale on top of 12pt bitmap (2× ≈ prior “10pt × 2” intent). */
static constexpr uint8_t kMenuTextSize = 2;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int32_t lin_map(int32_t x, int32_t a, int32_t b, int32_t c, int32_t d) {
  if (x <= a) {
    return c;
  }
  if (x >= b) {
    return d;
  }
  return c + (x - a) * (d - c) / (b - a);
}

/* Port of scroll_event_cb translate + opacity (LV_OPA 0..255). */
static void lv_example_scroll6_tx_and_opa(int32_t cont_h, int32_t cont_y_center, int32_t child_y_center, int16_t *out_tx,
                                          int32_t *out_row_alpha_0_255) {
  const int32_t r = cont_h * 7 / 10;
  int32_t diff_y = (int32_t)child_y_center - (int32_t)cont_y_center;
  if (diff_y < 0) {
    diff_y = -diff_y;
  }
  int32_t x = r;
  if (diff_y < r) {
    const uint32_t x_sqr = (uint32_t)r * (uint32_t)r - (uint32_t)diff_y * (uint32_t)diff_y;
    const float root = sqrtf((float)x_sqr);
    x = r - (int32_t)(root + 0.5f);
  }
  if (x < 0) {
    x = 0;
  }
  if (x > r) {
    x = r;
  }
  if (out_tx) {
    *out_tx = (int16_t)x;
  }
  if (out_row_alpha_0_255) {
    const int32_t opa = lin_map(x, 0, r, 0, 255);
    *out_row_alpha_0_255 = 255 - opa;
  }
}

void P4OverlayMenu::clearItems() {
  releaseCache();
  _n = 0;
  _home_list = false;
  _home_scroll_y = 0;
}

void P4OverlayMenu::begin(Arduino_GFX *gfx) {
  _gfx = gfx;
}

void P4OverlayMenu::releaseCache() {
  delete _canvas;
  _canvas = nullptr;
  _cache_ok = false;
  _cache_hi = -999;
  _cache_scroll = -9999;
}

void P4OverlayMenu::setScreenSize(int16_t w, int16_t h) {
  if (w != _sw || h != _sh) {
    releaseCache();
  }
  _sw = w;
  _sh = h;
  const int16_t ddim = (w > h) ? (int16_t)(w - h) : (int16_t)(h - w);
  _round_display = ddim <= 8;
}

void P4OverlayMenu::open() {
  _open = true;
  _cache_ok = false;
}

void P4OverlayMenu::close() {
  _open = false;
  releaseCache();
}

void P4OverlayMenu::setupTwoColumn(int id_left, int id_right, const char *left_lbl, const char *right_lbl,
                                   int16_t margin, int16_t gap) {
  clearItems();
  if (_sw <= 0 || _sh <= 0) {
    return;
  }
  int16_t inner_w = (int16_t)((_sw - 2 * margin - gap) / 2);
  int16_t h = (int16_t)(_sh - 2 * margin);
  if (inner_w < 40 || h < 40) {
    return;
  }
  addItem({id_left, margin, margin, inner_w, h, left_lbl});
  addItem({id_right, (int16_t)(margin + inner_w + gap), margin, inner_w, h, right_lbl});
}

void P4OverlayMenu::setupGrid2x2(int id_tl, int id_tr, int id_bl, int id_br, const char *lbl_tl, const char *lbl_tr,
                                 const char *lbl_bl, const char *lbl_br, int16_t margin, int16_t gap) {
  clearItems();
  if (_sw <= 0 || _sh <= 0) {
    return;
  }
  int16_t inner_w = (int16_t)((_sw - 2 * margin - gap) / 2);
  int16_t inner_h = (int16_t)((_sh - 2 * margin - gap) / 2);
  if (inner_w < 36 || inner_h < 36) {
    return;
  }
  addItem({id_tl, margin, margin, inner_w, inner_h, lbl_tl});
  addItem({id_tr, (int16_t)(margin + inner_w + gap), margin, inner_w, inner_h, lbl_tr});
  addItem({id_bl, margin, (int16_t)(margin + inner_h + gap), inner_w, inner_h, lbl_bl});
  addItem({id_br, (int16_t)(margin + inner_w + gap), (int16_t)(margin + inner_h + gap), inner_w, inner_h, lbl_br});
}

void P4OverlayMenu::setupHomeTranslateList(const int *ids, const char *const *labels, int count, int16_t margin) {
  clearItems();
  if (_sw <= 0 || _sh <= 0 || !ids || !labels || count <= 0 || count > P4_OVERLAY_MAX_ITEMS) {
    return;
  }
  _home_list = true;
  _home_marg = margin;
  /* Row height for 12pt font × kMenuTextSize (see FreeSansBold12pt7b yAdvance). */
  int16_t base = (int16_t)((_sh - 2 * margin) / 5);
  if (base < 40) {
    base = 40;
  }
  _home_row_h = base;
  if (_home_row_h < 104) {
    _home_row_h = 104;
  }
  if (_home_row_h > 150) {
    _home_row_h = 150;
  }
  _home_scroll_y = 0;
  for (int i = 0; i < count; i++) {
    _items[i].id = ids[i];
    _items[i].label = labels[i];
    _items[i].x = 0;
    _items[i].y = 0;
    _items[i].w = 0;
    _items[i].h = 0;
  }
  _n = count;
}

void P4OverlayMenu::homeListBeginDrag(int16_t y) {
  _home_drag_last_y = y;
}

void P4OverlayMenu::homeListDragTo(int16_t y) {
  if (!_home_list) {
    return;
  }
  const int16_t d = (int16_t)(y - _home_drag_last_y);
  _home_drag_last_y = y;
  /* Finger moves down → rows move down with it (ycf uses −scroll_y). */
  int32_t sy = (int32_t)_home_scroll_y - (int32_t)d;
  const int32_t smax = (_n > 1) ? (int32_t)(_home_row_h * (_n - 1)) : 0;
  if (sy < 0) {
    sy = 0;
  }
  if (sy > smax) {
    sy = smax;
  }
  _home_scroll_y = (int16_t)sy;
  _cache_ok = false;
}

void P4OverlayMenu::homeListSnap() {
  if (!_home_list || _n <= 0 || _home_row_h <= 0) {
    return;
  }
  int32_t idx = ((int32_t)_home_scroll_y + (int32_t)_home_row_h / 2) / (int32_t)_home_row_h;
  if (idx < 0) {
    idx = 0;
  }
  if (idx > _n - 1) {
    idx = _n - 1;
  }
  _home_scroll_y = (int16_t)(idx * (int32_t)_home_row_h);
  _cache_ok = false;
}

void P4OverlayMenu::homeListRowGeometry(int index, int16_t *out_x, int16_t *out_y, int16_t *out_w, int16_t *out_h,
                                        int16_t *out_tx) const {
  const int16_t cont_y_center = (int16_t)(_sh / 2);
  const float ycf = (float)cont_y_center + (float)index * (float)_home_row_h - (float)_home_scroll_y;
  const int16_t y_center_i = (int16_t)(ycf + 0.5f);
  int16_t tx = 0;
  lv_example_scroll6_tx_and_opa((int32_t)_sh, (int32_t)cont_y_center, (int32_t)y_center_i, &tx, nullptr);
  if (out_tx) {
    *out_tx = tx;
  }
  const int16_t y_top = (int16_t)(y_center_i - _home_row_h / 2);
  /* Full inner width like lv_obj_set_width(btn, lv_pct(100)); translate_x shifts only. */
  const int16_t rw = (int16_t)(_sw - 2 * _home_marg);
  const int16_t x0 = (int16_t)(_home_marg + tx);
  if (out_x) {
    *out_x = x0;
  }
  if (out_y) {
    *out_y = y_top;
  }
  if (out_w) {
    *out_w = rw;
  }
  if (out_h) {
    *out_h = _home_row_h;
  }
}

int P4OverlayMenu::hitHomeTranslateList(int16_t x, int16_t y) const {
  if (!_home_list || _n <= 0) {
    return -3;
  }
  for (int i = 0; i < _n; i++) {
    int16_t rx = 0, ry = 0, rw = 0, rh = 0, tx = 0;
    homeListRowGeometry(i, &rx, &ry, &rw, &rh, &tx);
    if (rw < 8 || rh < 8) {
      continue;
    }
    if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) {
      return _items[i].id;
    }
  }
  return -2;
}

int P4OverlayMenu::onRelease(int16_t x, int16_t y, int16_t drag_px) const {
  if (!_open || _n <= 0) {
    return -3;
  }
  if (_home_list) {
    const int kDragTap = 20;
    if (drag_px > kDragTap) {
      return -6;
    }
    return hitHomeTranslateList(x, y);
  }
  for (int i = 0; i < _n; i++) {
    const P4OverlayItemSpec &z = _items[i];
    if (x >= z.x && x < z.x + z.w && y >= z.y && y < z.y + z.h) {
      return z.id;
    }
  }
  return -2;
}

void P4OverlayMenu::paintHomeTranslateList(Arduino_GFX *target, int16_t highlight_item_id) {
  const uint16_t shade = rgb565(8, 8, 10);
  const uint16_t panel_fill = rgb565(14, 14, 18);
  const uint16_t border_col = rgb565(55, 58, 68);
  const uint16_t row_base = rgb565(28, 30, 36);
  const uint16_t row_hi = rgb565(48, 52, 62);

  const int16_t cx = (int16_t)(_sw / 2);
  const int16_t cy = (int16_t)(_sh / 2); /* cont_y_center in lv_example_scroll_6 */
  const int16_t sm = (_sw < _sh) ? _sw : _sh;
  const int16_t ddim = (_sw > _sh) ? (int16_t)(_sw - _sh) : (int16_t)(_sh - _sw);
  const bool round = (ddim <= 8);
  const int16_t r_outer = (int16_t)((sm / 2) - 2);
  const int16_t r_panel = (int16_t)(r_outer - 12);

  target->fillRect(0, 0, _sw, _sh, shade);

  if (round) {
    target->fillCircle(cx, cy, r_panel, panel_fill);
    target->drawCircle(cx, cy, r_panel, border_col);
    target->drawCircle((int16_t)(cx - 1), cy, r_panel, border_col);
  } else {
    const int16_t inset = 12;
    target->fillRoundRect(inset, inset, (int16_t)(_sw - 2 * inset), (int16_t)(_sh - 2 * inset), 18, panel_fill);
    target->drawRoundRect(inset, inset, (int16_t)(_sw - 2 * inset), (int16_t)(_sh - 2 * inset), 18, border_col);
  }

  target->setFont(&FreeSansBold12pt7b);
  target->setTextSize(kMenuTextSize);

  for (int i = 0; i < _n; i++) {
    int16_t rx = 0, ry = 0, rw = 0, rh = 0, tx = 0;
    homeListRowGeometry(i, &rx, &ry, &rw, &rh, &tx);

    const int16_t y_center_i = (int16_t)(ry + rh / 2);
    int32_t row_alpha = 255;
    lv_example_scroll6_tx_and_opa((int32_t)_sh, (int32_t)cy, (int32_t)y_center_i, nullptr, &row_alpha);
    if (row_alpha < 8) {
      continue;
    }
    if (ry + rh < -4 || ry > _sh + 4) {
      continue;
    }

    const bool hi = (highlight_item_id > -900 && _items[i].id == highlight_item_id);
    uint32_t br = 0, bg = 0, bb = 0;
    uint32_t er = 0, eg = 0, eb = 0;
    {
      const uint16_t base_c = hi ? row_hi : row_base;
      br = (uint32_t)((base_c >> 11) & 31U) * 255U / 31U;
      bg = (uint32_t)((base_c >> 5) & 63U) * 255U / 63U;
      bb = (uint32_t)(base_c & 31U) * 255U / 31U;
      const uint16_t edge_c = border_col;
      er = (uint32_t)((edge_c >> 11) & 31U) * 255U / 31U;
      eg = (uint32_t)((edge_c >> 5) & 63U) * 255U / 63U;
      eb = (uint32_t)(edge_c & 31U) * 255U / 31U;
    }
    const uint16_t fill565 =
        rgb565((uint8_t)(br * row_alpha / 255), (uint8_t)(bg * row_alpha / 255), (uint8_t)(bb * row_alpha / 255));
    const uint16_t edge565 =
        rgb565((uint8_t)(er * row_alpha / 255), (uint8_t)(eg * row_alpha / 255), (uint8_t)(eb * row_alpha / 255));

    target->fillRoundRect(rx, ry, rw, rh, 10, fill565);
    target->drawRoundRect(rx, ry, rw, rh, 10, edge565);

    if (_items[i].label && _items[i].label[0]) {
      int16_t bx = 0;
      int16_t by = 0;
      uint16_t tw = 0;
      uint16_t th = 0;
      target->getTextBounds(_items[i].label, 0, 0, &bx, &by, &tw, &th);
      const int32_t tr = (int32_t)(hi ? 235 : 130) * row_alpha / 255;
      const int32_t tg = (int32_t)(hi ? 238 : 135) * row_alpha / 255;
      const int32_t tb = (int32_t)(hi ? 245 : 145) * row_alpha / 255;
      target->setTextColor(rgb565((uint8_t)tr, (uint8_t)tg, (uint8_t)tb));
      int16_t txx = (int16_t)(rx + (rw - (int16_t)tw) / 2);
      int16_t tyy = (int16_t)(ry + (rh + (int16_t)th) / 2);
      if (txx < rx + 4) {
        txx = (int16_t)(rx + 4);
      }
      target->setCursor(txx, tyy);
      target->print(_items[i].label);
    }
  }
  target->setFont(nullptr);
}

void P4OverlayMenu::paintMenu(Arduino_GFX *target, int16_t highlight_item_id) {
  if (_home_list) {
    paintHomeTranslateList(target, highlight_item_id);
    return;
  }
  const uint16_t shade = 0x0000;
  const uint16_t panel_fill = 0x0000;
  const uint16_t amber_border = rgb565(120, 120, 120);
  const uint16_t btn_base = rgb565(44, 44, 44);
  const uint16_t btn_hi = rgb565(72, 72, 72);
  const uint16_t text_dim = rgb565(200, 200, 200);
  const uint16_t text_bright = rgb565(255, 255, 255);

  const int16_t cx = _sw / 2;
  const int16_t cy = _sh / 2;
  const int16_t sm = (_sw < _sh) ? _sw : _sh;
  const int16_t ddim = (_sw > _sh) ? (int16_t)(_sw - _sh) : (int16_t)(_sh - _sw);
  const bool round = (ddim <= 8);
  const int16_t r_outer = (int16_t)((sm / 2) - 2);
  const int16_t r_panel = (int16_t)(r_outer - 12);

  // Must fill the full canvas first; on round displays fillCircle leaves square corners undefined.
  target->fillRect(0, 0, _sw, _sh, shade);

  if (round) {
    target->fillCircle(cx, cy, r_panel, panel_fill);
    target->drawCircle(cx, cy, r_panel, amber_border);
    target->drawCircle((int16_t)(cx - 1), cy, r_panel, amber_border);
  } else {
    const int16_t inset = 12;
    target->fillRoundRect(inset, inset, (int16_t)(_sw - 2 * inset), (int16_t)(_sh - 2 * inset), 18, panel_fill);
    target->drawRoundRect(inset, inset, (int16_t)(_sw - 2 * inset), (int16_t)(_sh - 2 * inset), 18, amber_border);
  }

  target->setFont(&FreeSansBold12pt7b);
  target->setTextSize(kMenuTextSize);
  for (int i = 0; i < _n; i++) {
    const P4OverlayItemSpec &z = _items[i];
    const bool hi = (highlight_item_id > -900 && z.id == highlight_item_id);
    const uint16_t fill = hi ? btn_hi : btn_base;
    target->fillRoundRect(z.x, z.y, z.w, z.h, 12, fill);
    target->drawRoundRect(z.x, z.y, z.w, z.h, 12, amber_border);
    if (z.label && z.label[0]) {
      int16_t bx = 0;
      int16_t by = 0;
      uint16_t tw = 0;
      uint16_t th = 0;
      target->getTextBounds(z.label, 0, 0, &bx, &by, &tw, &th);
      int16_t tx = (int16_t)(z.x + (z.w - (int16_t)tw) / 2);
      int16_t ty = (int16_t)(z.y + (z.h + (int16_t)th) / 2);
      if (tx < z.x + 4) {
        tx = (int16_t)(z.x + 4);
      }
      target->setTextColor(hi ? text_bright : text_dim);
      target->setCursor(tx, ty);
      target->print(z.label);
    }
  }
  target->setFont(nullptr);
}

void P4OverlayMenu::draw(int16_t highlight_item_id) {
  if (!_open || !_gfx || _sw <= 0 || _sh <= 0) {
    return;
  }

  if (!_cache_ok || highlight_item_id != _cache_hi || (_home_list && _cache_scroll != _home_scroll_y)) {
    if (!_canvas) {
      _canvas = new Arduino_Canvas(_sw, _sh, _gfx);
      // Do not call panel begin() again — that re-inits MIPI and causes visible flicker.
      if (!_canvas || !_canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
        delete _canvas;
        _canvas = nullptr;
        paintMenu(_gfx, highlight_item_id);
        return;
      }
    }
    paintMenu(_canvas, highlight_item_id);
    if (_round_display) {
      uint16_t *p = _canvas->getFramebuffer();
      if (p) {
        p4_rgb565_mask_outside_circle(p, _sw, _sh, 0x0000);
      }
    }
    _cache_ok = true;
    _cache_hi = highlight_item_id;
    _cache_scroll = _home_scroll_y;
  }

  uint16_t *buf = _canvas->getFramebuffer();
  if (buf) {
    _gfx->draw16bitRGBBitmap(0, 0, buf, _sw, _sh);
  } else {
    paintMenu(_gfx, highlight_item_id);
  }
}
