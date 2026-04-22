#pragma once

/**
 * Shared layout / tuning for El Camino P4 gauge firmwares.
 * Center cluster needles and clock geometry align with TSPS-style LAYOUT_REF 1080 assets.
 *
 * Dial angle convention (layout / art / MPG math): 0° points along −X (9 o'clock, far left on the
 * horizontal through the hub). Angles increase clockwise on the dial face. Example: center efficiency
 * average MPG spans 45° at 0 MPG to 135° at 40 MPG (90° clockwise). Same “0° = horizontal left” idea
 * as tach user degrees below.
 */

#define EL_CAMINO_LINE_DRAW_STEPS_CENTER 500

#define EL_CAMINO_GAUGE_LAYOUT_REF_PX 1080

#define EL_CAMINO_NEEDLE_HUB_X_FRAC 0.5f
#define EL_CAMINO_NEEDLE_HUB_Y_FRAC 0.42f

/* Tach: 0° = needle along horizontal left; 0 RPM @ 30°, 6000 RPM @ 150° (120° sweep).
 * Hub, needle length, and tick radii are fractions of the center view edge (square buffer side). */
#define EL_CAMINO_CENTER_TACH_RPM_CLAMP 6000
#define EL_CAMINO_CENTER_TACH_RPM_RATIO_REF 6000.0f
#define EL_CAMINO_CENTER_TACH_USER_DEG_0_RPM 30.0f
#define EL_CAMINO_CENTER_TACH_USER_DEG_MAX_RPM 150.0f
#define EL_CAMINO_CENTER_TACH_HUB_X_FRAC_OF_VIEW 0.5f
#define EL_CAMINO_CENTER_TACH_HUB_Y_FRAC_OF_VIEW 0.5f
#define EL_CAMINO_CENTER_TACH_NEEDLE_LEN_FRAC_OF_VIEW 0.36f
#define EL_CAMINO_CENTER_TACH_NEEDLE_RGB565 0xF9A6

#define EL_CAMINO_CENTER_TACH_NEEDLE_LINE_W 3
/* Tick ring: inner/outer as fraction of view size (cw), then subtract TICK_RADIUS_TRIM_PX from each. */
#define EL_CAMINO_CENTER_TACH_TICK_OUTER_FRAC_OF_VIEW 0.415f
#define EL_CAMINO_CENTER_TACH_TICK_INNER_MAJOR_FRAC_OF_VIEW 0.36f
#define EL_CAMINO_CENTER_TACH_TICK_INNER_MINOR_FRAC_OF_VIEW 0.375f
#define EL_CAMINO_CENTER_TACH_TICK_RADIUS_TRIM_PX 50
#define EL_CAMINO_CENTER_TACH_TICK_MAJOR_RGB565 0xCE59
#define EL_CAMINO_CENTER_TACH_TICK_MINOR_RGB565 0x8430
#define EL_CAMINO_CENTER_TACH_TICK_LAST_MAJOR_RGB565 0xF800
#define EL_CAMINO_CENTER_TACH_TICK_MAJOR_THICK 2
#define EL_CAMINO_CENTER_TACH_TICK_MINOR_THICK 1

/* Clock sprite pivot (fraction of sprite w/h). Must match dashboard gauge_screen._draw_clock_sprite_hand
 * default pivot [0.5, 0.94] — hub at the narrow end of the needle bitmap, not the image center. */
#define EL_CAMINO_CENTER_CLOCK_PIVOT_X_FRAC 0.5f
#define EL_CAMINO_CENTER_CLOCK_PIVOT_Y_FRAC 0.94f
#define EL_CAMINO_CENTER_CLOCK_ANGLE_OFFSET_DEG 0.0f

/* Line-mode clock hands (hub at dial center) */
#define EL_CAMINO_CENTER_CLOCK_LINE_HUB_X_FRAC 0.5f
#define EL_CAMINO_CENTER_CLOCK_LINE_HUB_Y_FRAC 0.5f
#define EL_CAMINO_CENTER_CLOCK_LINE_MINUTE_LEN_FRAC 0.42f
#define EL_CAMINO_CENTER_CLOCK_LINE_HOUR_LEN_FRAC 0.28f
#define EL_CAMINO_CENTER_CLOCK_LINE_RGB565 0xFFFF
#define EL_CAMINO_CENTER_CLOCK_LINE_MINUTE_THICK 2
#define EL_CAMINO_CENTER_CLOCK_LINE_HOUR_THICK 3

#define EL_CAMINO_CENTER_BG_RGB565 0x0000 /* #000 */
