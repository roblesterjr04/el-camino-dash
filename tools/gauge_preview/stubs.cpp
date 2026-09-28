// Host stand-ins for the few platform hooks the gauge code and LVGL config expect.
#include <stdint.h>
#include <stdlib.h>

static uint32_t g_ms = 0;
extern "C" uint32_t millis(void) { return g_ms; }
void host_set_ms(uint32_t ms) { g_ms = ms; }
extern "C" void *lv_psram_malloc(size_t n) { return malloc(n); }
extern "C" void lv_psram_free(void *p) { free(p); }
extern "C" void *lv_psram_realloc(void *p, size_t n) { return realloc(p, n); }
