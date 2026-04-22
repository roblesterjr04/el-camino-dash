#include "p4_round_mask.h"

void p4_rgb565_mask_outside_circle(uint16_t *buf, int16_t w, int16_t h, uint16_t outside_rgb565) {
  if (!buf || w <= 0 || h <= 0) {
    return;
  }
  const int32_t cx = (int32_t)w / 2;
  const int32_t cy = (int32_t)h / 2;
  const int32_t sm = w < h ? (int32_t)w : (int32_t)h;
  const int32_t r = (sm - 4) / 2;
  if (r <= 0) {
    return;
  }
  const int32_t r2 = r * r;
  for (int32_t y = 0; y < (int32_t)h; y++) {
    uint16_t *row = buf + (int32_t)w * y;
    const int32_t dy = y - cy;
    const int32_t dy2 = dy * dy;
    for (int32_t x = 0; x < (int32_t)w; x++) {
      const int32_t dx = x - cx;
      if (dx * dx + dy2 > r2) {
        row[x] = outside_rgb565;
      }
    }
  }
}
