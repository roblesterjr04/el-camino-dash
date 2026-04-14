#include "speedo_fallout.h"

#include <stdio.h>

static uint16_t fo_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | ((uint16_t)b >> 3));
}

static void seg_bar(Arduino_GFX *g, int16_t x, int16_t y, int n, float fuel_pct, uint16_t on_c, uint16_t off_c) {
  const int16_t seg_w = 14;
  const int16_t gap = 3;
  int fi = (int)((fuel_pct / 100.0f) * 10.0f);
  if (fi < 0) {
    fi = 0;
  }
  if (fi > 10) {
    fi = 10;
  }
  for (int i = 0; i < n; i++) {
    g->fillRect((int16_t)(x + i * (seg_w + gap)), y, seg_w, 10, i < fi ? on_c : off_c);
  }
}

void speedo_draw_fallout(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, float mph, const SensorDerived &d) {
  (void)mph;
  const uint16_t fg = fo_rgb(0, 255, 60);
  const uint16_t dim = fo_rgb(0, 90, 30);
  gfx->setFont(nullptr);
  gfx->fillScreen(0x0000);
  gfx->setTextColor(fg);
  gfx->setTextSize(1);
  gfx->setCursor((int16_t)(screen_w / 2 - 56), 28);
  gfx->print("- STATUS -");

  gfx->setTextColor(dim);
  gfx->setCursor(40, 80);
  gfx->print("[ vehicle outline ]");
  gfx->drawRect(80, 100, (int16_t)(screen_w - 160), 100, fg);

  gfx->setTextColor(fg);
  gfx->setCursor(32, 230);
  gfx->print("FUEL");
  seg_bar(gfx, 120, 226, 10, d.fuel_pct, fg, dim);

  char line[32];
  snprintf(line, sizeof(line), "%.0f%%", (double)d.fuel_pct);
  gfx->setCursor((int16_t)(screen_w - 100), 230);
  gfx->print(line);

  gfx->setCursor(32, 276);
  gfx->print("RADIO");
  gfx->setCursor((int16_t)(screen_w - 140), 276);
  gfx->print("-- MHz");

  gfx->setCursor(32, 320);
  gfx->print("TEMP");
  snprintf(line, sizeof(line), "%.0f F", (double)d.coolant_temp_f);
  gfx->setCursor((int16_t)(screen_w - 120), 320);
  gfx->print(line);

  gfx->setTextColor(dim);
  gfx->setCursor(24, (int16_t)(screen_h - 36));
  gfx->print(".. DATA . MENU . STAT");
}
