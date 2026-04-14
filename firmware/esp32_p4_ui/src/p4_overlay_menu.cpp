#include "p4_overlay_menu.h"

#include <Arduino.h>
#include <Arduino_DataBus.h>
#include <canvas/Arduino_Canvas.h>
#include <cstring>

#include "FreeSansBold10pt7b.h"
#include "p4_round_mask.h"

// Pi menu_overlay.py _paint_overlay (round): shade (14,10,4,195), panel (40,30,12,225), border (188,148,54,235)
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void P4OverlayMenu::begin(Arduino_GFX *gfx) {
  _gfx = gfx;
}

void P4OverlayMenu::releaseCache() {
  delete _canvas;
  _canvas = nullptr;
  _cache_ok = false;
  _cache_hi = -999;
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

int P4OverlayMenu::onRelease(int16_t x, int16_t y) const {
  if (!_open || _n <= 0) {
    return -3;
  }
  for (int i = 0; i < _n; i++) {
    const P4OverlayItemSpec &z = _items[i];
    if (x >= z.x && x < z.x + z.w && y >= z.y && y < z.y + z.h) {
      return z.id;
    }
  }
  return -2;
}

void P4OverlayMenu::paintMenu(Arduino_GFX *target, int16_t highlight_item_id) {
  const uint16_t shade = 0x0000;
  const uint16_t panel_fill = rgb565(28, 28, 28);
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

  target->setFont(&FreeSansBold10pt7b);
  target->setTextSize(1);
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

  if (!_cache_ok || highlight_item_id != _cache_hi) {
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
  }

  uint16_t *buf = _canvas->getFramebuffer();
  if (buf) {
    _gfx->draw16bitRGBBitmap(0, 0, buf, _sw, _sh);
  } else {
    paintMenu(_gfx, highlight_item_id);
  }
}
