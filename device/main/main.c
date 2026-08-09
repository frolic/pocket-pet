#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "esp_lvgl_port.h"
#include "bsp/display.h"
#include "device_debug.h"
#include "device_wifi.h"
#include "frolic_app.h"
#include "pet.h"
#include "watchface.h"

void app_main(void)
{
    device_debug_start();
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* LVGL on core 1, away from the wifi stack on core 0 — a starved flush
       pipeline jams the panel SPI queue (dropped flushes, then watchdog). */
    bsp_display_cfg_t display_config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
        }};
    display_config.lvgl_port_cfg.task_affinity = 1;
    bsp_display_start_with_config(&display_config);
    bsp_display_lock(0);
    frolic_app_init(lv_screen_active());
    bsp_display_unlock();

#ifndef FROLIC_DISABLE_WIFI
    device_wifi_start();
#endif
}
