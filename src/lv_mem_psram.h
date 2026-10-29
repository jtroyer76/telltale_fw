// lv_mem_psram.h — LVGL's heap (LV_MEM_CUSTOM) in PSRAM. Its objects are small, so plain malloc
// would put them in internal RAM, which WiFi needs: with every screen built there wasn't enough
// left for a phone to join the setup hotspot. Wired up in CMakeLists.txt and sdkconfig.
#pragma once

#include <stdlib.h>
#include "esp_heap_caps.h"

static inline void *lv_psram_alloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }

static inline void *lv_psram_realloc(void *p, size_t size) { return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM); }
