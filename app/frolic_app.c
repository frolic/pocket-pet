#include "lvgl.h"
#include "frolic_app.h"
#include "button_source.h"
#include "display_sleep.h"
#include "power_button.h"
#include "game_config.h"
#include "step_source.h"
#include "watchface.h"
#include "pet.h"

static uint32_t last_total;
static bool button_was_held;

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
#define LISTEN_HOLD_MS 700

static uint32_t press_started_tick;
static bool listening_active;

static void poll_button(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    bool held = button_source_held();

    if (held && !button_was_held) press_started_tick = lv_tick_get();

    if (held && !listening_active && !display_sleep_is_asleep() &&
        lv_tick_elaps(press_started_tick) >= LISTEN_HOLD_MS) {
        listening_active = true;
        watchface_set_recording(true);
        pet_listen_start();
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

static void poll_power_button(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (!power_button_pressed()) return;
    if (display_sleep_is_asleep()) {
        display_sleep_wake();
    } else {
        display_sleep_sleep_now();
    }
}

void frolic_app_init(lv_obj_t *parent)
{
    watchface_create(parent);
    watchface_set_steps(step_source_total());
    lv_timer_create(poll_steps, 400, NULL);
    lv_timer_create(poll_button, 50, NULL);
    lv_timer_create(poll_power_button, 150, NULL);
    display_sleep_init(10000);
}
