#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "frolic_app.h"

void app_main(void)
{
    bsp_display_start();
    bsp_display_lock(0);
    frolic_app_init(lv_screen_active());
    bsp_display_unlock();
}
