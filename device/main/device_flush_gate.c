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
 * Closed by the sleep loop before light sleep and by raw-draw diagnostics.
 * Closing is deterministic: the LVGL task renders and flushes everything
 * pending (banner, setup modal), signals completion, and only then is the
 * pipeline sealed — a timed grace truncated slow first paints mid-frame.
 */

static bool closed;
static SemaphoreHandle_t render_done;

static void render_and_signal_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    /* Seal a COMPLETE frame: repaint everything, twice. Sporadic SPI flush
       drops (~1% of strips under boot congestion) leave stale strips that a
       dirty-area render never revisits — frozen on glass for the whole
       gated window. The full invalidate heals all earlier damage, and since
       the scene is static across the passes, a strip stays wrong only if it
       drops in BOTH (~0.01%). */
    for (int pass = 0; pass < 2; pass++) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(NULL);
    }
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

static void heal_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    lv_obj_invalidate(lv_screen_active());
}

void device_flush_gate_open(void)
{
    if (!closed) return;
    bsp_display_lock(0);
    lv_display_enable_invalidation(lv_display_get_default(), true);
    /* Catch-up: repaint everything that changed while sealed. */
    lv_obj_invalidate(lv_screen_active());
    /* Heal passes: the catch-up burst overflows the SPI queue (5-30% strip
       drops per full-frame burst on this board) and static regions never
       repaint on their own — black chunks would sit on glass indefinitely.
       Re-invalidate after the queue drains; a strip dropped in the heal
       pass still shows the previous pass's identical pixels. Harmless if
       the gate recloses first: invalidation-disabled makes these no-ops. */
    lv_timer_t *heal_soon = lv_timer_create(heal_cb, 400, NULL);
    lv_timer_set_repeat_count(heal_soon, 1);
    lv_timer_t *heal_late = lv_timer_create(heal_cb, 1200, NULL);
    lv_timer_set_repeat_count(heal_late, 1);
    bsp_display_unlock();
    closed = false;
    printf("flush_gate: open\n");
}
