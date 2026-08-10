#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "device_flush_gate.h"

/*
 * The radio/display invariant, enforced mechanically: closing the gate
 * disables LVGL invalidation — no invalidation means no render, no render
 * means no flush, so nothing can touch the panel while the radio runs.
 * The close/open calls are fused to esp_wifi_start/stop inside device_wifi;
 * the radio cannot come up around the gate. Timers and input keep running
 * (only rendering is frozen), so buttons stay live while gated.
 */

static bool closed;

void device_flush_gate_close(void)
{
    if (closed) return;
    /* Let any just-queued UI (banner, setup modal) land first. */
    vTaskDelay(pdMS_TO_TICKS(250));
    bsp_display_lock(0);
    lv_display_enable_invalidation(lv_display_get_default(), false);
    bsp_display_unlock();
    /* Drain SPI transactions already in the panel queue. */
    vTaskDelay(pdMS_TO_TICKS(80));
    closed = true;
    printf("flush_gate: closed\n");
}

void device_flush_gate_open(void)
{
    if (!closed) return;
    bsp_display_lock(0);
    lv_display_enable_invalidation(lv_display_get_default(), true);
    /* Catch-up: repaint everything that changed while sealed. */
    lv_obj_invalidate(lv_screen_active());
    bsp_display_unlock();
    closed = false;
    printf("flush_gate: open\n");
}
