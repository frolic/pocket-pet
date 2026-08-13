#include "esp_heap_caps.h"
#include "lvgl.h"

/*
 * LVGL heap in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC). Widget objects, styles
 * and animation bookkeeping tolerate PSRAM latency; the render DRAW BUFFER
 * is unaffected (allocated internal-DMA by esp_lvgl_port — landmine #1's
 * wait-semantics fix is what makes that safe, not this file). Frees the
 * 64KB internal builtin pool so wifi + the BLE stack fit together.
 * Internal fallback keeps LVGL alive if PSRAM ever runs dry.
 */

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    void *pointer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (pointer == NULL) pointer = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    return pointer;
}

void *lv_realloc_core(void *original, size_t size)
{
    return heap_caps_realloc(original, size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void *pointer)
{
    heap_caps_free(pointer);
}

void lv_mem_monitor_core(lv_mem_monitor_t *monitor)
{
    lv_memset(monitor, 0, sizeof(lv_mem_monitor_t));
    monitor->free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    monitor->total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
