#include "display_sleep.h"
#include "pet.h"

static lv_obj_t *blanket;
static lv_timer_t *watch_timer;
static uint32_t sleep_timeout_ms;
static bool asleep;
static bool settling;
static uint32_t settle_started_tick;
static uint32_t pet_asleep_since_tick;
static void (*hardware_cb)(bool on);

/* Bedtime choreography: the pet visibly settles before the screen fades. */
#define SLEEP_POSE_LINGER_MS 1200
#define SETTLE_CAP_MS 8000

static void go_to_sleep(void)
{
    asleep = true;
    settling = false;
    lv_obj_remove_flag(blanket, LV_OBJ_FLAG_HIDDEN);
    pet_freeze(true);
    if (hardware_cb != NULL) hardware_cb(false);
}

static void watch_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (asleep) return;
    bool inactive = lv_display_get_inactive_time(NULL) > sleep_timeout_ms;
    if (!settling) {
        if (inactive) {
            /* Bedtime: he settles down first; the screen follows. */
            settling = true;
            settle_started_tick = lv_tick_get();
            pet_asleep_since_tick = 0;
            pet_sleep_now();
        }
        return;
    }
    if (!inactive) {
        /* Activity during the wind-down: screen stays on. */
        settling = false;
        return;
    }
    if (pet_is_sleeping()) {
        if (pet_asleep_since_tick == 0) pet_asleep_since_tick = lv_tick_get();
        if (lv_tick_elaps(pet_asleep_since_tick) >= SLEEP_POSE_LINGER_MS) {
            go_to_sleep();
            return;
        }
    }
    /* He might be mid-interaction; don't stall the screen forever. */
    if (lv_tick_elaps(settle_started_tick) > SETTLE_CAP_MS) go_to_sleep();
}

void display_sleep_init(uint32_t timeout_ms)
{
    sleep_timeout_ms = timeout_ms;
    /* Top-layer blanket: blacks the sim and swallows touches on device,
       where the panel keeps sensing even with the AMOLED dark. */
    blanket = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(blanket);
    lv_obj_set_size(blanket, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(blanket, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(blanket, LV_OPA_COVER, 0);
    lv_obj_add_flag(blanket, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(blanket, LV_OBJ_FLAG_HIDDEN);
    watch_timer = lv_timer_create(watch_tick, 300, NULL);
}

void display_sleep_set_hw_cb(void (*hw_cb)(bool on))
{
    hardware_cb = hw_cb;
}

bool display_sleep_is_asleep(void)
{
    return asleep;
}

void display_sleep_poke(void)
{
    lv_display_trigger_activity(NULL);
}

void display_sleep_sleep_now(void)
{
    if (!asleep) go_to_sleep();
}

void display_sleep_wake(void)
{
    if (!asleep) return;
    asleep = false;
    lv_obj_add_flag(blanket, LV_OBJ_FLAG_HIDDEN);
    pet_freeze(false);
    if (hardware_cb != NULL) hardware_cb(true);
    lv_display_trigger_activity(NULL);
}
