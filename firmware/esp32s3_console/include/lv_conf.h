// LVGL for the console: only its font format and glyph lookup are used (the console draws into its own 4-bit
// framebuffer, oled_gfx.cpp); nothing here starts LVGL's display or task machinery.
#pragma once
#define LV_COLOR_DEPTH 16
#define LV_MEM_SIZE (8 * 1024)
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_DEFAULT NULL
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
