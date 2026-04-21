#pragma once

class Arduino_GFX;

// Copy embedded boot image (assets/bootscreen/boot3.png → bootscreen_rgb565_*_gen.h) into the
// MIPI DSI RGB565 framebuffer via getFramebuffer(), then flush. Resolution is chosen from
// TSPS_/LITMUS_ panel flags (800², 720², or 1024×600); full-frame memcpy when it matches the panel.
void elc_boot_splash_framebuffer_write(Arduino_GFX *gfx);
