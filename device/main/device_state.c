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
#include "display_sleep.h"
#include "watchface.h"
#include "device_wifi.h"
#include "device_flush_gate.h"
#include "device_touch_raw.h"
#include "esp_timer.h"
#include "power_button.h"

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
static uint32_t radio_state_entered_ms;

/* Radio states must be transient (PORTAL excepted — the user is driving).
   A hung window otherwise leaves the gate closed forever: frozen banner,
   frozen clock, dead watch face. */
#define RADIO_STATE_TIMEOUT_MS 60000

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
    /* Setup keeps the screen on; the gate means a timeout could never
       redraw the wake. */
    display_sleep_set_hold(current == DEVICE_STATE_PORTAL ||
                           current == DEVICE_STATE_SYNC_VISIBLE);
    if (!in_lvgl_context) bsp_display_unlock();
}

/* Transition under the mutex; UI application is deferred to the caller
   (never take the display lock while holding state_mutex — the display
   callback arrives on the LVGL task in the opposite lock order). */
static bool is_radio_state(device_state_t state)
{
    return state == DEVICE_STATE_SYNCING || state == DEVICE_STATE_SYNC_VISIBLE;
}

static bool transition(device_state_t next)
{
    if (next == current) return false;
    printf("device_state: %s -> %s\n", state_name(current), state_name(next));
    /* The watchdog clock marks the radio SESSION start, not the latest state
       hop: SYNCING<->SYNC_VISIBLE flaps (every PWR press while stuck syncing)
       must not keep deferring the force-release. */
    if (is_radio_state(next) && !is_radio_state(current)) {
        radio_state_entered_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
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

static void portal_wake_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    display_sleep_wake();
}

/* The flush gate freezes rendering but not input; still, hit-test raw
   touch (simple and robust) plus PWR. Cancel tears the portal down and
   returns to normal operation — no reboot. */
static void portal_input_task(void *arg)
{
    (void)arg;
    int cancel_min_y = watchface_setup_modal_cancel_min_y();
    /* Arm only after the entry touch has fully lifted plus a beat —
       rapid taps were cancelling the portal instantly. */
    int idle_polls = 0;
    while (current == DEVICE_STATE_PORTAL && idle_polls < 15) {
        int x, y;
        vTaskDelay(pdMS_TO_TICKS(100));
        if (device_touch_raw_get(&x, &y)) idle_polls = 0;
        else idle_polls++;
    }
    while (current == DEVICE_STATE_PORTAL) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int x, y;
        if (power_button_pressed() ||
            (device_touch_raw_get(&x, &y) && y >= cancel_min_y)) {
            printf("device_state: setup cancelled\n");
            device_wifi_portal_exit();
            break;
        }
    }
    vTaskDelete(NULL);
}

void device_state_portal(void)
{
    bool changed = false;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    changed = transition(DEVICE_STATE_PORTAL);
    xSemaphoreGive(state_mutex);
    if (changed) {
        apply_ui(false);
        /* Entering setup with the screen dark: wake it (LVGL context — the
           wake path renders). Runs before the flush gate's landing render
           seals the pipeline. */
        bsp_display_lock(0);
        lv_timer_t *wake_timer = lv_timer_create(portal_wake_cb, 5, NULL);
        lv_timer_set_repeat_count(wake_timer, 1);
        bsp_display_unlock();
        xTaskCreate(portal_input_task, "portalin", 3072, NULL, 3, NULL);
    }
}

void device_state_portal_exit(void)
{
    bool changed = false;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (current == DEVICE_STATE_PORTAL) changed = transition(DEVICE_STATE_ACTIVE);
    xSemaphoreGive(state_mutex);
    if (changed) apply_ui(false);
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
        if (current == DEVICE_STATE_SYNCING || current == DEVICE_STATE_SYNC_VISIBLE) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (now - radio_state_entered_ms > RADIO_STATE_TIMEOUT_MS) {
                printf("device_state: WATCHDOG — stuck in %s, force-releasing\n",
                       state_name(current));
                device_wifi_force_stop();
                device_flush_gate_open();
                device_state_release_radio();
            }
        }
    }
}

void device_state_init(void)
{
    state_mutex = xSemaphoreCreateMutex();
    xTaskCreate(housekeeping_task, "statehk", 3072, NULL, 2, NULL);
}
