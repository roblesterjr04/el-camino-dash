#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL allocator (lv_conf.h LV_MEM_CUSTOM): PSRAM first, internal RAM as fallback. */
void *lv_psram_malloc(size_t size);
void lv_psram_free(void *p);
void *lv_psram_realloc(void *p, size_t size);

#ifdef __cplusplus
}
#endif
