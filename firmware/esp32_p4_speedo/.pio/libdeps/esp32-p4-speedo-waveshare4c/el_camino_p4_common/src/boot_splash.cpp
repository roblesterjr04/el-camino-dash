#include "boot_splash.h"

#include <Arduino_GFX_Library.h>
#include <string.h>

// One header per panel resolution (see scripts/export_bootscreen_rgb565.py).
#if (defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4) || (defined(LITMUS_WAVESHARE_3C4) && LITMUS_WAVESHARE_3C4)
#include <bootscreen_rgb565_800_gen.h>
#elif (defined(TSPS_PANEL_1024X600) && TSPS_PANEL_1024X600) || (defined(LITMUS_PANEL_1024X600) && LITMUS_PANEL_1024X600)
#include <bootscreen_rgb565_1024x600_gen.h>
#else
#include <bootscreen_rgb565_720_gen.h>
#endif

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32P4)

void elc_boot_splash_framebuffer_write(Arduino_GFX *gfx) {
  if (!gfx) {
    return;
  }
  auto *dsi = static_cast<Arduino_DSI_Display *>(gfx);
  uint16_t *fb = dsi->getFramebuffer();
  const int pw = gfx->width();
  const int ph = gfx->height();
  const int sw = ELC_BOOT_SPLASH_W;
  const int sh = ELC_BOOT_SPLASH_H;

  if (!fb || pw <= 0 || ph <= 0) {
    return;
  }

  if (pw == sw && ph == sh) {
    memcpy(fb, elc_boot_splash_rgb565, (size_t)sw * (size_t)sh * sizeof(uint16_t));
    gfx->flush(true);
    return;
  }

  if (sw > pw || sh > ph) {
    gfx->fillScreen(0x0000);
    gfx->flush(true);
    return;
  }

  memset(fb, 0, (size_t)pw * (size_t)ph * sizeof(uint16_t));

  const int x0 = (pw - sw) / 2;
  const int y0 = (ph - sh) / 2;
  const uint16_t *src = elc_boot_splash_rgb565;

  for (int y = 0; y < sh; y++) {
    uint16_t *dst = fb + (size_t)(y0 + y) * (size_t)pw + (size_t)x0;
    memcpy(dst, src + (size_t)y * (size_t)sw, (size_t)sw * sizeof(uint16_t));
  }

  gfx->flush(true);
}

#else

void elc_boot_splash_framebuffer_write(Arduino_GFX *) {}

#endif
