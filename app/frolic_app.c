#include "lvgl.h"
#include "frolic_app.h"
#include "button_source.h"
#include "battery_source.h"
#include "display_sleep.h"
#include "power_button.h"
#include "game_config.h"
#include "step_source.h"
#include "watchface.h"
#include "pet.h"

static uint32_t last_total;
static bool button_was_held;

static void poll_battery(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    watchface_set_battery(battery_source_percent(), battery_source_charging());
}

static void poll_steps(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    uint32_t total = step_source_total();
    if (total == last_total) return;
    watchface_set_steps(total);
    if (total / STEP_GOAL > last_total / STEP_GOAL) {
        pet_celebrate(PET_CELEBRATION_BREATH);
    } else {
        pet_notice_steps(total - last_total);
    }
    last_total = total;
}

/* Recording begins only after a deliberate hold — a stray press does nothing. */
#define LISTEN_HOLD_MS 300

static uint32_t press_started_tick;
static bool listening_active;

static void poll_button(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    bool held = button_source_held();

    if (held && !button_was_held) press_started_tick = lv_tick_get();

    if (held && lv_tick_elaps(press_started_tick) >= LISTEN_HOLD_MS) {
        if (display_sleep_is_asleep()) {
            /* Held while dark: wake the screen; keep holding to record. */
            display_sleep_wake();
            press_started_tick = lv_tick_get();
        } else if (!listening_active) {
            listening_active = true;
            watchface_set_recording(true);
            pet_listen_start();
        }
    }
    if (!held && button_was_held) {
        display_sleep_poke();
        if (listening_active) {
            listening_active = false;
            watchface_set_recording(false);
            pet_listen_end();
        }
    }
    if (held) display_sleep_poke();
    button_was_held = held;
}

/* PWR while lit: single press sleeps, double press (within the window)
   boosts to full brightness for the rest of this screen-on session. The
   sleep therefore waits out the window before landing. */
#define PWR_DOUBLE_TAP_MS 500

static bool pwr_sleep_pending;
static uint32_t pwr_first_tap_tick;

static void poll_power_button(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (pwr_sleep_pending &&
        lv_tick_elaps(pwr_first_tap_tick) > PWR_DOUBLE_TAP_MS) {
        pwr_sleep_pending = false;
        display_sleep_sleep_now();
        return;
    }
    if (!power_button_pressed()) return;
    if (display_sleep_is_asleep()) {
        display_sleep_wake();
        return;
    }
    if (pwr_sleep_pending) {
        pwr_sleep_pending = false;
        display_sleep_boost(100);
    } else {
        pwr_sleep_pending = true;
        pwr_first_tap_tick = lv_tick_get();
    }
}

void frolic_app_init(lv_obj_t *parent)
{
    watchface_create(parent);
    watchface_set_steps(step_source_total());
    lv_timer_create(poll_steps, 400, NULL);
    lv_timer_ready(lv_timer_create(poll_battery, 30000, NULL));
    lv_timer_create(poll_button, 50, NULL);
    lv_timer_create(poll_power_button, 150, NULL);
    display_sleep_init(10000);
}
