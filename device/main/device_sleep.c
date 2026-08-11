#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "device_sleep.h"
#include "device_state.h"
#include "device_axp2101.h"
#include "device_flush_gate.h"
#include "device_step_source.h"
#include "display_sleep.h"
#include "power_button.h"

/*
 * Manual light sleep for the dark hours — the full-day-battery lever.
 *
 * Deliberately NOT esp_pm automatic light sleep (tickless idle): tickless
 * measurably drops panel SPI flushes on this board even while fully awake
 * with every pm lock held (~3-5 failed flushes/second during rendering,
 * A/B-verified against the DFS-only build), and automatic sleep can engage
 * anywhere the locks allow, which twice ended in display corruption. Here
 * sleep is an explicit, owned loop that runs only when the state machine
 * says DOZING on battery, with rendering sealed first — so a light sleep
 * can never cut an in-flight SPI flush or I2C transaction, and the awake
 * system stays bit-identical to the verified DFS-only firmware.
 *
 * Loop shape: sleep in ~40ms quanta (the accel sampling period — steps keep
 * counting), read the accelerometer each wake, poll the AXP2101 power key
 * every 4th wake (~160ms latency), and wake instantly on BOOT via GPIO
 * low-level wakeup. Slept time is credited back to the FreeRTOS tick with
 * xTaskCatchUpTicks once a second, then a short awake window lets due tasks
 * (OTA scheduler, housekeeping watchdog, NVS persist, idle/TWDT) run.
 *
 * Requires CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=n: with it on, a startup
 * hook arms hardware sleep-sel isolation on every pad and the first sleep
 * entry floats the QSPI panel bus + GPIO13, latching the panel (the frozen
 * corruption of the earlier attempts). With it off, pads hold state.
 */

#define BOOT_BUTTON GPIO_NUM_0
#define QUANTUM_MS 40
#define PWR_POLL_QUANTA 4
#define CATCH_UP_MS 1000
#define AWAKE_WINDOW_MS 10
#define ENTRY_POLL_MS 500

static void wake_display_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    display_sleep_wake();
}

/* The wake render must run on the LVGL task (same pattern as the portal). */
static void schedule_display_wake(void)
{
    bsp_display_lock(0);
    lv_timer_t *wake_timer = lv_timer_create(wake_display_cb, 5, NULL);
    lv_timer_set_repeat_count(wake_timer, 1);
    bsp_display_unlock();
}

static bool sleep_eligible(void)
{
    return device_state_get() == DEVICE_STATE_DOZING && !axp2101_vbus_present();
}

static void catch_up(int64_t *credit_us)
{
    if (*credit_us <= 0) return;
    xTaskCatchUpTicks(pdMS_TO_TICKS((uint32_t)(*credit_us / 1000)));
    *credit_us = 0;
}

static void sleep_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ENTRY_POLL_MS));
        if (!sleep_eligible()) continue;

        /* Seal rendering: no flush can start once the gate is closed, so a
           light sleep can never freeze the panel bus mid-transfer. The close
           handshake renders + drains via the LVGL task first. */
        device_flush_gate_close();
        if (!sleep_eligible()) {
            /* Lost the race (woke / radio window / USB): hand the gate back
               unless a radio state owns it now. */
            if (device_state_get() == DEVICE_STATE_DOZING ||
                device_state_get() == DEVICE_STATE_ACTIVE) {
                device_flush_gate_open();
            }
            continue;
        }

        /* BOOT wakes the chip out of light sleep instantly. Re-armed every
           entry: gpio_config() elsewhere resets the pin's trigger type. */
        gpio_wakeup_enable(BOOT_BUTTON, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        printf("device_sleep: dark loop begin\n");
        step_source_external_pacing(true);
        int64_t credit_us = 0;
        uint32_t quantum = 0;
        bool wake_display = false;

        while (sleep_eligible()) {
            esp_sleep_enable_timer_wakeup(QUANTUM_MS * 1000);
            int64_t before = esp_timer_get_time();
            esp_light_sleep_start();
            credit_us += esp_timer_get_time() - before;

            /* Sample every wake: steps keep counting through the night.
               Doubles as the I2C quiesce point — it serializes behind any
               transaction a briefly-scheduled task left in flight. */
            step_source_sample_now();

            if (gpio_get_level(BOOT_BUTTON) == 0) {
                wake_display = true;
                break;
            }
            if (++quantum % PWR_POLL_QUANTA == 0 && power_button_pressed()) {
                wake_display = true;
                break;
            }
            if (credit_us >= CATCH_UP_MS * 1000) {
                /* Credit the slept time to the RTOS tick so wall-clock task
                   scheduling (OTA windows, watchdogs, persist) stays honest,
                   then yield briefly so everything due — and the idle tasks,
                   which feed the task watchdog — gets to run. */
                catch_up(&credit_us);
                vTaskDelay(pdMS_TO_TICKS(AWAKE_WINDOW_MS));
            }
        }

        catch_up(&credit_us);
        step_source_external_pacing(false);
        printf("device_sleep: dark loop end (%s)\n",
               wake_display ? "button wake" : "state/usb");

        /* Gate handback: radio states (SYNCING/PORTAL) close the gate on
           their own behalf and must keep it; every other exit returns to
           normal rendering. Button wakes need the gate open BEFORE the wake
           frame renders. */
        device_state_t state = device_state_get();
        if (wake_display || state == DEVICE_STATE_DOZING ||
            state == DEVICE_STATE_ACTIVE) {
            device_flush_gate_open();
        }
        if (wake_display) schedule_display_wake();
    }
}

void device_sleep_init(void)
{
    /* Above the LVGL/step tasks: wake handling preempts routine polls, and
       the loop only yields CPU at its own explicit points. */
    xTaskCreate(sleep_task, "sleep", 3072, NULL, 5, NULL);
    printf("device_sleep: manual light sleep armed (DOZING on battery)\n");
}
