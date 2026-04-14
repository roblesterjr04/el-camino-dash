#include "center_fallout.h"

#include <el_camino_speed.h>
#include <stdio.h>

static uint16_t fo_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | ((uint16_t)b >> 3));
}

static void draw_seg_bar(Arduino_GFX *g, int16_t x, int16_t y, int16_t seg_w, int16_t gap, int n, int filled,
                         uint16_t on_c, uint16_t off_c) {
  for (int i = 0; i < n; i++) {
    const int16_t sx = (int16_t)(x + i * (seg_w + gap));
    g->fillRect(sx, y, seg_w, (int16_t)(seg_w / 2 + 2), i < filled ? on_c : off_c);
  }
}

void center_draw_fallout(Arduino_GFX *gfx, int16_t screen_w, int16_t screen_h, const CenterGaugeInputs &in) {
  gfx->setFont(nullptr);
  gfx->fillScreen(0x0000);

  const uint16_t fg = fo_rgb(0, 255, 60);
  const uint16_t dim = fo_rgb(0, 90, 30);

  char line[48];
  gfx->setTextColor(fg);
  gfx->setTextSize(1);
  gfx->setCursor((int16_t)(screen_w / 2 - 54), 28);
  gfx->print("- SPEED -");

  const float mph = elc_speed_rpm_to_mph(in.speed_rpm, 2.1f, 1.0f);
  int mph_i = (int)(mph + 0.5f);
  if (mph_i < 0) {
    mph_i = 0;
  }
  if (mph_i > 999) {
    mph_i = 999;
  }
  snprintf(line, sizeof(line), "%03d", mph_i);

  gfx->setCursor(28, 108);
  gfx->setTextColor(dim);
  gfx->print("1  2  3");
  const int gn = in.gear_num;
  if (gn >= 1 && gn <= 3) {
    const int16_t box_x = (int16_t)(24 + (gn - 1) * 28);
    gfx->drawRect((int16_t)(box_x - 2), 96, 22, 26, fg);
  }

  gfx->setTextColor(fg);
  gfx->setTextSize(2);
  gfx->setCursor((int16_t)(screen_w / 2 - 8), 96);
  gfx->print(line);
  gfx->setCursor((int16_t)(screen_w / 2 + 52), 96);
  gfx->setTextSize(1);
  gfx->print("MPH");

  gfx->setTextSize(1);
  gfx->setCursor(28, 200);
  gfx->print("RPM");
  gfx->setCursor((int16_t)(screen_w - 120), 200);
  snprintf(line, sizeof(line), "%u", (unsigned)in.engine_rpm);
  gfx->print(line);

  gfx->setCursor(28, 236);
  gfx->print("AP");
  int ap = (int)((in.system_amp + 100.0f) / 200.0f * 10.0f);
  if (ap < 0) {
    ap = 0;
  }
  if (ap > 10) {
    ap = 10;
  }
  draw_seg_bar(gfx, 110, 232, 14, 3, 10, ap, fg, dim);

  gfx->setCursor(28, 276);
  gfx->print("TRIP");
  gfx->setCursor((int16_t)(screen_w - 120), 276);
  snprintf(line, sizeof(line), "%.0f MI", (double)in.trip_mi);
  gfx->print(line);

  gfx->setCursor(28, 312);
  gfx->print("ODO");
  gfx->setCursor((int16_t)(screen_w - 140), 312);
  snprintf(line, sizeof(line), "%.0f MI", (double)in.odometer_mi);
  gfx->print(line);

  gfx->setTextColor(dim);
  gfx->setCursor(24, (int16_t)(screen_h - 36));
  gfx->print(".. DATA . MENU . STAT");
}
