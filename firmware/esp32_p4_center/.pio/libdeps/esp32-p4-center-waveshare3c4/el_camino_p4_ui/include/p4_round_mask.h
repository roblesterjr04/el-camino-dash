#pragma once

#include <stdint.h>

// Force pixels outside the inscribed circle (centered, ~full diameter) to outside_rgb565.
// Use so square framebuffers match a round physical panel (#000 in corners).
void p4_rgb565_mask_outside_circle(uint16_t *buf, int16_t w, int16_t h, uint16_t outside_rgb565);
