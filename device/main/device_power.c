#include <stdio.h>
#include "esp_pm.h"
#include "device_power.h"

/*
 * Clock/light-sleep switch, driven by the device state machine: full speed
 * for anything interactive or radio-bearing, scaled clocks plus automatic
 * light sleep only when the state machine says the watch is dark on battery.
 */

static esp_pm_lock_handle_t cpu_lock;
static esp_pm_lock_handle_t freq_lock;
static esp_pm_lock_handle_t sleep_lock;
static bool locks_held;
static bool ready;

void device_power_set_full(bool full)
{
    if (!ready || full == locks_held) return;
    if (full) {
        esp_pm_lock_acquire(cpu_lock);
        esp_pm_lock_acquire(freq_lock);
        esp_pm_lock_acquire(sleep_lock);
    } else {
        esp_pm_lock_release(cpu_lock);
        esp_pm_lock_release(freq_lock);
        esp_pm_lock_release(sleep_lock);
    }
    locks_held = full;
}

void device_power_init(void)
{
    esp_pm_config_t config = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    esp_err_t result = esp_pm_configure(&config);
    if (result != ESP_OK) {
        printf("device_power: pm configure failed (%s)\n", esp_err_to_name(result));
        return;
    }
    /* All three: CPU pinned (LVGL renders miss flush deadlines at 80MHz),
       APB pinned (QSPI/I2C peripheral clocks), no light sleep. */
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "cpu", &cpu_lock);
    esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "screen", &freq_lock);
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "awake", &sleep_lock);
    /* Boot is interactive: start at full speed. */
    esp_pm_lock_acquire(cpu_lock);
    esp_pm_lock_acquire(freq_lock);
    esp_pm_lock_acquire(sleep_lock);
    locks_held = true;
    ready = true;
    printf("device_power: dfs 80-240MHz, light sleep when dark on battery\n");
}
