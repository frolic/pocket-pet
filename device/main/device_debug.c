#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "device_debug.h"

static volatile uint32_t flush_failures;
static uint32_t flush_failures_reported;
static vprintf_like_t previous_vprintf;

static int counting_vprintf(const char *format, va_list args)
{
    if (strstr(format, "spi transmit (queue) color failed") != NULL) {
        flush_failures++;
        /* Let one in 500 through so the failure stays visible without the
           spam itself becoming the bottleneck. */
        if (flush_failures % 500 != 1) return 0;
    }
    return previous_vprintf(format, args);
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTERRUPT-WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK-WATCHDOG";
    case ESP_RST_WDT: return "OTHER-WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    default: return "other";
    }
}

static void heartbeat_task(void *arg)
{
    while (true) {
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        uint32_t failures = flush_failures;
        printf("HB up=%llds heap=%uk min=%uk psram=%uk flushfail=%u(+%u) wifi=%d\n",
               esp_timer_get_time() / 1000000,
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
               (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
               (unsigned)failures, (unsigned)(failures - flush_failures_reported),
               (int)mode);
        /* Self-heal: a sustained flush-failure storm means the SPI pipeline
           wedged (it never recovers on its own) — restart clears it. */
        static int storm_beats;
        if (failures - flush_failures_reported > 100) {
            if (++storm_beats >= 3) {
                printf("SELF-HEAL: flush pipeline wedged (%u failures) — restarting\n",
                       (unsigned)failures);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else {
            storm_beats = 0;
        }
        flush_failures_reported = failures;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void device_debug_start(void)
{
    printf("BOOT reset_reason=%s\n", reset_reason_name(esp_reset_reason()));
    previous_vprintf = esp_log_set_vprintf(counting_vprintf);
    xTaskCreatePinnedToCore(heartbeat_task, "debug_hb", 3072, NULL, 2, NULL, 1);
}
