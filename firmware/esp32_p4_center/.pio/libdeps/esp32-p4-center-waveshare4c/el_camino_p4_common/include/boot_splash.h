#pragma once

class Arduino_GFX;

// Copy embedded boot image (assets/bootscreen/boot2.png → bootscreen_rgb565_gen.h) into the
// MIPI DSI RGB565 framebuffer via getFramebuffer(), then flush. Clears to black outside the
// centered square (ELC_BOOT_SPLASH_EDGE).
void elc_boot_splash_framebuffer_write(Arduino_GFX *gfx);
