#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "device_wifi.h"
#include "frolic_app.h"

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    bsp_display_start();
    bsp_display_lock(0);
    frolic_app_init(lv_screen_active());
    bsp_display_unlock();

#ifndef FROLIC_DISABLE_WIFI
    device_wifi_start();
#endif
}
