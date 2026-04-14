#include "tsps_fallout.h"

#include <stdio.h>

static uint16_t fo_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | ((uint16_t)b >> 3));
}

static void hp_bar(Arduino_GFX *g, int16_t x, int16_t y, int n, float hp_pct, uint16_t on_c, uint16_t off_c) {
  const int16_t seg_w = 14;
  const int16_t gap = 3;
  int fi = (int)((hp_pct / 100.0f) * 10.0f);
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

void tsps_draw_fallout(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, const SensorDerived &s) {
  const uint16_t fg = fo_rgb(0, 255, 60);
  const uint16_t dim = fo_rgb(0, 90, 30);
  gfx->setFont(nullptr);
  gfx->fillScreen(0x0000);
  gfx->setTextColor(fg);
  gfx->setTextSize(1);
  gfx->setCursor((int16_t)(screen_w / 2 - 40), 28);
  gfx->print("- HP -");

  float hp = 100.0f - (s.oil_psi * 0.15f);
  if (hp < 0.0f) {
    hp = 0.0f;
  }
  if (hp > 100.0f) {
    hp = 100.0f;
  }
  hp_bar(gfx, 40, 120, 10, hp, fg, dim);

  char line[32];
  snprintf(line, sizeof(line), "%.0f%%", (double)hp);
  gfx->setCursor((int16_t)(screen_w - 100), 118);
  gfx->print(line);

  gfx->setCursor(32, 200);
  gfx->print("RAD");
  gfx->setCursor((int16_t)(screen_w - 120), 200);
  gfx->print("0.00 R");

  gfx->setCursor(32, 250);
  gfx->print("NAV");
  gfx->setCursor(120, 250);
  gfx->print("> NW");

  gfx->setCursor(32, 300);
  gfx->print("WASTELAND RD.");

  gfx->setTextColor(dim);
  gfx->setCursor(24, (int16_t)(screen_h - 36));
  gfx->print("MAP . DATA . MENU . STAT");
}
