#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "lvgl.h"
#include "device_state.h"
#include "device_power.h"
#include "device_axp2101.h"
#include "pet.h"
#include "watchface.h"
#include "device_wifi.h"
#include "device_touch_raw.h"
#include "power_button.h"
#include "esp_system.h"

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
   sleep callback) must pass in_lvgl_context=true to skip the display lock.

   The zero-render invariant is enforced mechanically by the flush gate
   (device_flush_gate.c), fused to esp_wifi_start/stop — this function only
   applies the visible UI (banner/modal/pause); timers and input keep
   running while gated. */
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
        banner = NULL; /* the setup modal replaces the banner */
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
    watchface_show_setup_modal(current == DEVICE_STATE_PORTAL);
    if (!in_lvgl_context) bsp_display_unlock();
}

/* Transition under the mutex; UI application is deferred to the caller
   (never take the display lock while holding state_mutex — the display
   callback arrives on the LVGL task in the opposite lock order). */
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
    bool changed = false;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (asleep) {
        if (current == DEVICE_STATE_ACTIVE) changed = transition(DEVICE_STATE_DOZING);
        else if (current == DEVICE_STATE_SYNC_VISIBLE) changed = transition(DEVICE_STATE_SYNCING);
    } else {
        if (current == DEVICE_STATE_DOZING) changed = transition(DEVICE_STATE_ACTIVE);
        else if (current == DEVICE_STATE_SYNCING) changed = transition(DEVICE_STATE_SYNC_VISIBLE);
    }
    xSemaphoreGive(state_mutex);
    if (changed) apply_ui(true);
}

bool device_state_request_radio(void)
{
    bool granted = false;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (current == DEVICE_STATE_DOZING) granted = transition(DEVICE_STATE_SYNCING);
    xSemaphoreGive(state_mutex);
    if (granted) apply_ui(false);
    return granted;
}

void device_state_release_radio(void)
{
    bool changed = false;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (current == DEVICE_STATE_SYNCING) changed = transition(DEVICE_STATE_DOZING);
    else if (current == DEVICE_STATE_SYNC_VISIBLE) changed = transition(DEVICE_STATE_ACTIVE);
    xSemaphoreGive(state_mutex);
    if (changed) apply_ui(false);
}

void device_state_boot_sync(void)
{
    /* Boot-time clock sync: screen is up, so show the truce explicitly. */
    bool changed = false;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    changed = transition(DEVICE_STATE_SYNC_VISIBLE);
    xSemaphoreGive(state_mutex);
    if (changed) apply_ui(false);
}

/* With the renderer stopped, LVGL's input pipeline is dead too: watch the
   touch controller and power button raw. Cancel (or PWR) reboots back to
   normal operation — the portal state is terminal either way. */
static void portal_input_task(void *arg)
{
    (void)arg;
    int cancel_min_y = watchface_setup_modal_cancel_min_y();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int x, y;
        if (power_button_pressed() ||
            (device_touch_raw_get(&x, &y) && y >= cancel_min_y)) {
            printf("device_state: setup cancelled — rebooting\n");
            esp_restart();
        }
    }
}

void device_state_portal(void)
{
    bool changed = false;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    changed = transition(DEVICE_STATE_PORTAL);
    xSemaphoreGive(state_mutex);
    if (changed) {
        apply_ui(false);
        xTaskCreate(portal_input_task, "portalin", 3072, NULL, 3, NULL);
    }
}

/* Slow housekeeping in a plain task: LVGL and lock-waits must never run on
   the shared esp_timer task (LVGL's own tick lives there). */
static void housekeeping_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (current == DEVICE_STATE_DOZING) apply_power();
        if (current == DEVICE_STATE_ACTIVE) {
            bsp_display_lock(0);
            watchface_set_wifi_offline(device_wifi_is_offline());
            bsp_display_unlock();
        }
    }
}

void device_state_init(void)
{
    state_mutex = xSemaphoreCreateMutex();
    xTaskCreate(housekeeping_task, "statehk", 3072, NULL, 2, NULL);
}
