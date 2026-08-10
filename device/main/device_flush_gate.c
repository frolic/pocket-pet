#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "device_flush_gate.h"

/*
 * The radio/display invariant, enforced mechanically: closing the gate
 * disables LVGL invalidation — no invalidation means no render, no render
 * means no flush, so nothing can touch the panel while the radio runs.
 * The close/open calls are fused to esp_wifi_start/stop inside device_wifi.
 * Closing is deterministic: the LVGL task renders and flushes everything
 * pending (banner, setup modal), signals completion, and only then is the
 * pipeline sealed — a timed grace truncated slow first paints mid-frame.
 */

static bool closed;
static SemaphoreHandle_t render_done;

static void render_and_signal_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    lv_refr_now(NULL);
    xSemaphoreGive(render_done);
}

void device_flush_gate_close(void)
{
    if (closed) return;
    if (render_done == NULL) render_done = xSemaphoreCreateBinary();

    bsp_display_lock(0);
    lv_timer_t *render_timer = lv_timer_create(render_and_signal_cb, 10, NULL);
    lv_timer_set_repeat_count(render_timer, 1);
    bsp_display_unlock();

    if (xSemaphoreTake(render_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        printf("flush_gate: render-before-close timed out\n");
    }
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
