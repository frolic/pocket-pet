#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "device_state.h"
#include "device_power.h"
#include "device_axp2101.h"
#include "pet.h"
#include "watchface.h"
#include "device_wifi.h"

/*
 * The device mode state machine — single owner of the radio/display truce.
 * Radio and animation corrupt each other on this board, so "radio up with a
 * live screen" must be unrepresentable. Radio is only granted from DOZING
 * (screen provably dark and static); waking mid-window lands in
 * SYNC_VISIBLE, which shows a banner over a deliberately frozen scene.
 *
 *   ACTIVE        screen on, animations, radio forbidden, full clocks
 *   DOZING        dark, frozen, radio may be requested, clocks relaxed
 *   SYNCING       dark, radio up, full clocks
 *   SYNC_VISIBLE  woke mid-window: banner + frozen pet, radio up
 *   PORTAL        wifi setup mode: banner + frozen pet, terminal
 */

static device_state_t current = DEVICE_STATE_ACTIVE;
static SemaphoreHandle_t state_mutex;
static esp_timer_handle_t pm_timer;

static const char *state_name(device_state_t state)
{
    switch (state) {
    case DEVICE_STATE_ACTIVE: return "ACTIVE";
    case DEVICE_STATE_DOZING: return "DOZING";
    case DEVICE_STATE_SYNCING: return "SYNCING";
    case DEVICE_STATE_SYNC_VISIBLE: return "SYNC_VISIBLE";
    case DEVICE_STATE_PORTAL: return "PORTAL";
    }
    return "?";
}

/* Clocks: full unless dozing on battery. */
static void apply_power(void)
{
    device_power_set_full(current != DEVICE_STATE_DOZING || axp2101_vbus_present());
}

/* UI invariants for the new state. Callers in LVGL context (the display
   sleep callback) must pass in_lvgl_context=true to skip the display lock. */
static void apply_ui(bool in_lvgl_context)
{
    bool paused;
    const char *banner;
    switch (current) {
    case DEVICE_STATE_SYNC_VISIBLE:
        paused = true;
        banner = "SYNCING";
        break;
    case DEVICE_STATE_PORTAL:
        paused = true;
        banner = "WIFI SETUP";
        break;
    case DEVICE_STATE_ACTIVE:
        paused = false;
        banner = NULL;
        break;
    default:
        /* Dark states: the display-sleep freeze already owns the scene. */
        return;
    }
    if (!in_lvgl_context) bsp_display_lock(0);
    pet_set_paused(paused);
    watchface_set_banner(banner);
    if (!in_lvgl_context) bsp_display_unlock();
}

static void transition(device_state_t next, bool in_lvgl_context)
{
    if (next == current) return;
    printf("device_state: %s -> %s\n", state_name(current), state_name(next));
    current = next;
    apply_power();
    apply_ui(in_lvgl_context);
}

device_state_t device_state_get(void)
{
    return current;
}

void device_state_report_display(bool asleep)
{
    /* Called from LVGL context (display-sleep fade completion / wake). */
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (asleep) {
        if (current == DEVICE_STATE_ACTIVE) transition(DEVICE_STATE_DOZING, true);
        else if (current == DEVICE_STATE_SYNC_VISIBLE) transition(DEVICE_STATE_SYNCING, true);
    } else {
        if (current == DEVICE_STATE_DOZING) transition(DEVICE_STATE_ACTIVE, true);
        else if (current == DEVICE_STATE_SYNCING) transition(DEVICE_STATE_SYNC_VISIBLE, true);
    }
    xSemaphoreGive(state_mutex);
}

bool device_state_request_radio(void)
{
    bool granted = false;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (current == DEVICE_STATE_DOZING) {
        transition(DEVICE_STATE_SYNCING, false);
        granted = true;
    }
    xSemaphoreGive(state_mutex);
    return granted;
}

void device_state_release_radio(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (current == DEVICE_STATE_SYNCING) transition(DEVICE_STATE_DOZING, false);
    else if (current == DEVICE_STATE_SYNC_VISIBLE) transition(DEVICE_STATE_ACTIVE, false);
    xSemaphoreGive(state_mutex);
}

void device_state_boot_sync(void)
{
    /* Boot-time clock sync: screen is up, so show the truce explicitly. */
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    transition(DEVICE_STATE_SYNC_VISIBLE, false);
    xSemaphoreGive(state_mutex);
}

void device_state_portal(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    transition(DEVICE_STATE_PORTAL, false);
    xSemaphoreGive(state_mutex);
}

static void pm_timer_cb(void *arg)
{
    (void)arg;
    /* Re-evaluate clocks while dozing: USB plug/unplug changes the answer. */
    if (current == DEVICE_STATE_DOZING) apply_power();
    /* Keep the offline icon honest while the screen is up. */
    if (current == DEVICE_STATE_ACTIVE) {
        bsp_display_lock(0);
        watchface_set_wifi_offline(device_wifi_is_offline());
        bsp_display_unlock();
    }
}

void device_state_init(void)
{
    state_mutex = xSemaphoreCreateMutex();
    const esp_timer_create_args_t timer_args = {
        .callback = pm_timer_cb,
        .name = "state_pm",
    };
    esp_timer_create(&timer_args, &pm_timer);
    esp_timer_start_periodic(pm_timer, 5 * 1000 * 1000);
}
