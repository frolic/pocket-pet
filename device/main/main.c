#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "esp_lvgl_port.h"
#include "bsp/display.h"
#include "device_debug.h"
#include "device_leash.h"
#include "device_wifi.h"
#include "device_ota.h"
#include "device_power.h"
#include "device_rtc.h"
#include "device_sleep.h"
#include "device_state.h"
#include "device_debug_console.h"
#include "display_sleep.h"
#include "render_test_main.h"
#include "frolic_app.h"
#include "pet.h"
#include "watchface.h"

/* AMOLED: brightness 0 is pixels-off — so the fade-out ramp ends with the
   panel effectively sleeping. */
static void lvgl_liveness_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    device_debug_note_lvgl_alive();
}

#ifndef FROLIC_DISABLE_WIFI
/* Radio status onto the watchface: red = radio up but nothing established
   (or offline with the radio down), pulsing yellow = seeking, white =
   connected. Runs on the LVGL task, so no display lock needed. */
static void wifi_icon_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    watchface_wifi_state_t state;
    if (device_wifi_radio_active()) {
        if (device_wifi_is_connected()) state = WATCHFACE_WIFI_CONNECTED;
        else if (device_wifi_in_portal()) state = WATCHFACE_WIFI_STRANDED;
        else state = WATCHFACE_WIFI_CONNECTING;
    } else {
        state = device_wifi_is_offline() ? WATCHFACE_WIFI_OFFLINE
                                         : WATCHFACE_WIFI_HIDDEN;
    }
    watchface_set_wifi(state);
}
#endif

static void display_dim(uint8_t brightness_percent)
{
    /* The fade animation calls every frame; only touch the panel on change. */
    static uint8_t last_sent = 255;
    if (brightness_percent == last_sent) return;
    last_sent = brightness_percent;
    bsp_display_brightness_set(brightness_percent);
}

void app_main(void)
{
#ifdef FROLIC_RENDER_TEST
    /* Diagnostic build: characterize the raw draw path, no app, no LVGL. */
    render_test_main();
    return;
#endif
    device_debug_start();
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* LVGL on core 1, away from the wifi stack on core 0.

       Internal-DMA draw buffer, and this is now safe ON PURPOSE: the
       vendored BSP flush waits for real SPI completion (trans-done), so the
       buffer is never reused mid-DMA — which was the actual cause of the
       historic "internal buffer stripes the panel" rule. PSRAM only ever
       looked clean because spi_master bounce-copied it through a contiguous
       internal DMA alloc per flush, and THAT alloc failing under wifi heap
       fragmentation (ESP_ERR_NO_MEM) was the entire flush-failure storm.
       Characterized end-to-end by the FROLIC_RENDER_TEST harness:
       PSRAM buffer under wifi = 100% flush failures; internal = zero. */
    bsp_display_cfg_t display_config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }};
    display_config.lvgl_port_cfg.task_affinity = 1;
    /* GPIO13 must be left at its power-on default and never configured: the
   clean demo never touched it; configuring it (drive OR float) corrupts
   the panel. No gpio_config for pin 13 anywhere. */
    bsp_display_start_with_config(&display_config);
    bsp_display_brightness_set(80);
    bsp_display_lock(0);
    frolic_app_init(lv_screen_active());
    lv_timer_create(lvgl_liveness_tick, 500, NULL);
    display_sleep_set_dim_cb(display_dim);
    display_sleep_set_state_cb(device_state_report_display);
#ifndef FROLIC_DISABLE_WIFI
    watchface_set_wifi_tap_cb(device_wifi_request_portal);
    lv_timer_create(wifi_icon_tick, 500, NULL);
#endif
    bsp_display_unlock();

    /* London time regardless of wifi (SNTP normally sets this up). */
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();
    /* Battery-backed RTC: an honest clock from second one on every boot
       where the chip kept power — the wifi boot sync then skips itself. */
    device_rtc_restore();

    device_power_init();
    device_state_init();
    device_sleep_init();
    device_debug_console_start();
#ifndef FROLIC_DISABLE_WIFI
    device_wifi_start();
    device_ota_start();
#endif
    /* After wifi: its core structures demand internal RAM ("alloc pp wdev
       funcs" fails otherwise); the BLE host lives in PSRAM and can wait. */
    device_leash_init();
}
