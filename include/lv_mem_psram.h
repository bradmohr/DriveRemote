#pragma once
// LVGL heap hooks: keep widget memory in PSRAM so internal RAM stays free for
// Wi-Fi, the RGB bounce buffers and the LVGL draw buffer.
#include <stddef.h>
#include "esp_heap_caps.h"

static inline void *lv_psram_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static inline void *lv_psram_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

static inline void lv_psram_free(void *ptr)
{
    heap_caps_free(ptr);
}
