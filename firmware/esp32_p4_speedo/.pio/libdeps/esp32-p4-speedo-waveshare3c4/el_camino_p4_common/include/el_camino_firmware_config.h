#pragma once

/**
 * Shared layout / tuning for El Camino P4 gauge firmwares.
 * Center cluster needles and clock geometry align with TSPS-style LAYOUT_REF 1080 assets.
 */

#define EL_CAMINO_LINE_DRAW_STEPS_CENTER 500

#define EL_CAMINO_GAUGE_LAYOUT_REF_PX 1080

#define EL_CAMINO_NEEDLE_HUB_X_FRAC 0.5f
#define EL_CAMINO_NEEDLE_HUB_Y_FRAC 0.42f

/* Tach: 0–8000 RPM sweep (user degrees), similar arc to TSPS sub-gauges */
#define EL_CAMINO_CENTER_TACH_RPM_CLAMP 8000
#define EL_CAMINO_CENTER_TACH_RPM_RATIO_REF 8000.0f
#define EL_CAMINO_CENTER_TACH_ANGLE_START_DEG 45.0f
#define EL_CAMINO_CENTER_TACH_ANGLE_END_DEG 315.0f
#define EL_CAMINO_CENTER_TACH_LEN_FRAC 0.35f
#define EL_CAMINO_CENTER_TACH_NEEDLE_RGB565 0xF9A6

#define EL_CAMINO_CENTER_TACH_NEEDLE_LINE_W 3

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
