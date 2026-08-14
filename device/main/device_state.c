#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "device_state.h"
#include "device_power.h"
#include "device_axp2101.h"

/*
 * The device mode state machine.
 *
 *   ACTIVE   screen on, animations, full clocks
 *   DOZING   dark, frozen scene, clocks relaxed on battery,
 *            light-sleep eligible (device_sleep.c)
 *
 * The BLE radio follows these states from device_familiar.c: advertising
 * and sessions while ACTIVE or on USB power, torn down when DOZING on
 * battery so the sleep loop can engage.
 */

static device_state_t current = DEVICE_STATE_ACTIVE;
static SemaphoreHandle_t state_mutex;

static const char *state_name(device_state_t state)
{
    switch (state) {
    case DEVICE_STATE_ACTIVE: return "ACTIVE";
    case DEVICE_STATE_DOZING: return "DOZING";
    }
    return "?";
}

/* Clocks: full unless dozing on battery. */
static void apply_power(void)
{
    device_power_set_full(current != DEVICE_STATE_DOZING || axp2101_vbus_present());
}

static bool transition(device_state_t next)
{
    if (next == current) return false;
    printf("device_state: %s -> %s\n", state_name(current), state_name(next));
    current = next;
    apply_power();
    return true;
}

device_state_t device_state_get(void)
{
    return current;
}

void device_state_report_display(bool asleep)
{
    /* Called from LVGL context (display-sleep fade completion / wake). */
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    transition(asleep ? DEVICE_STATE_DOZING : DEVICE_STATE_ACTIVE);
    xSemaphoreGive(state_mutex);
}

/* Slow housekeeping in a plain task: re-applies the power profile while
   dozing so a USB plug/unplug in the dark changes the clocks. */
static void housekeeping_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (current == DEVICE_STATE_DOZING) apply_power();
    }
}

void device_state_init(void)
{
    state_mutex = xSemaphoreCreateMutex();
    xTaskCreate(housekeeping_task, "statehk", 3072, NULL, 2, NULL);
}
