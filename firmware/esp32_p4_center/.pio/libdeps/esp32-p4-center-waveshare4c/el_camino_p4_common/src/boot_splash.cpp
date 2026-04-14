#include "boot_splash.h"

#include <Arduino_GFX_Library.h>
#include <string.h>

#include <bootscreen_rgb565_gen.h>

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32P4)

void elc_boot_splash_framebuffer_write(Arduino_GFX *gfx) {
  if (!gfx) {
    return;
  }
  auto *dsi = static_cast<Arduino_DSI_Display *>(gfx);
  uint16_t *fb = dsi->getFramebuffer();
  const int pw = gfx->width();
  const int ph = gfx->height();
  const int edge = ELC_BOOT_SPLASH_EDGE;

  if (!fb || pw <= 0 || ph <= 0) {
    return;
  }
  if (edge > pw || edge > ph) {
    gfx->fillScreen(0x0000);
    gfx->flush(true);
    return;
  }

  memset(fb, 0, (size_t)pw * (size_t)ph * sizeof(uint16_t));

  const int x0 = (pw - edge) / 2;
  const int y0 = (ph - edge) / 2;
  const uint16_t *src = elc_boot_splash_rgb565;

  for (int y = 0; y < edge; y++) {
    uint16_t *dst = fb + (size_t)(y0 + y) * (size_t)pw + (size_t)x0;
    memcpy(dst, src + (size_t)y * (size_t)edge, (size_t)edge * sizeof(uint16_t));
  }

  gfx->flush(true);
}

#else

void elc_boot_splash_framebuffer_write(Arduino_GFX *) {}

#endif
