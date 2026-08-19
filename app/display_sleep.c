#include "display_sleep.h"
#include "pet.h"

static lv_obj_t *blanket;
static lv_timer_t *watch_timer;
static uint32_t sleep_timeout_ms;
static bool asleep;
static bool settling;
static uint32_t settle_started_tick;
static uint32_t pet_asleep_since_tick;
static void (*dim_cb)(uint8_t brightness_percent);
static void (*state_cb)(bool asleep);
static bool hold;

/* Bedtime choreography: the pet visibly settles before the screen fades. */
#define SLEEP_POSE_LINGER_MS 1200
#define SETTLE_CAP_MS 8000

/* Fade-out: panel brightness ramps down while the blanket cross-fades in,
   gray first, then gray to black. One animation drives both. */
#define FADE_MS 1400
#define FADE_QUICK_MS 700 /* explicit sleep (PWR press): snappy response */
#define FADE_GRAY 0x3A3A3E
static uint8_t awake_brightness = 30;
/* Boost (PWR double-tap): full brightness until the screen next sleeps. */
static uint8_t boost_brightness;

static uint8_t lit_brightness(void)
{
    return boost_brightness != 0 ? boost_brightness : awake_brightness;
}

void display_sleep_boost(uint8_t percent)
{
    boost_brightness = percent;
    if (dim_cb != NULL && !display_sleep_is_asleep()) dim_cb(lit_brightness());
}

/* With a hardware dimmer (device), the fade is brightness-only: zero
   flushes, so the SPI queue never floods (full-screen blanket animation
   was overrunning the panel — the root of the tearing). Without one
   (sim), the blanket cross-fades in software. */
static void fade_exec(void *var, int32_t value)
{
    LV_UNUSED(var);
    if (dim_cb != NULL) {
        dim_cb((uint8_t)(lit_brightness() * (510 - value) / 510));
        return;
    }
    if (value <= 255) {
        lv_obj_set_style_bg_color(blanket, lv_color_hex(FADE_GRAY), 0);
        lv_obj_set_style_bg_opa(blanket, (lv_opa_t)value, 0);
    } else {
        lv_obj_set_style_bg_color(
            blanket,
            lv_color_mix(lv_color_black(), lv_color_hex(FADE_GRAY),
                         (uint8_t)(value - 255)),
            0);
        lv_obj_set_style_bg_opa(blanket, LV_OPA_COVER, 0);
    }
}

static void fade_done(lv_anim_t *anim)
{
    LV_UNUSED(anim);
    pet_freeze(true);
    /* Panel is dark (or the sim blanket landed): snap the blanket solid. */
    lv_obj_set_style_bg_color(blanket, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(blanket, LV_OPA_COVER, 0);
    if (dim_cb != NULL) dim_cb(0);
    boost_brightness = 0; /* boost lasts one screen-on session */
    if (state_cb != NULL) state_cb(true);
}

static void go_to_sleep(uint32_t fade_ms)
{
    asleep = true;
    settling = false;
    /* Blanket goes visible-but-clear immediately so touches during the
       fade are swallowed; the pet keeps breathing until the fade lands. */
    lv_obj_set_style_bg_opa(blanket, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(blanket, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, blanket);
    lv_anim_set_exec_cb(&anim, fade_exec);
    lv_anim_set_values(&anim, 0, 510);
    lv_anim_set_duration(&anim, fade_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&anim, fade_done);
    lv_anim_start(&anim);
}

static void watch_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (asleep || hold) return;
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
            go_to_sleep(FADE_MS);
            return;
        }
    }
    /* He might be mid-interaction; don't stall the screen forever. */
    if (lv_tick_elaps(settle_started_tick) > SETTLE_CAP_MS) go_to_sleep(FADE_MS);
}

void display_sleep_init(uint32_t timeout_ms)
{
    sleep_timeout_ms = timeout_ms;
    /* Top-layer blanket: fades the sim and swallows touches on device,
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

void display_sleep_set_dim_cb(void (*cb)(uint8_t brightness_percent))
{
    dim_cb = cb;
}

void display_sleep_set_state_cb(void (*cb)(bool asleep))
{
    state_cb = cb;
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
    if (!asleep) go_to_sleep(FADE_QUICK_MS);
}

static void wake_backlight_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (dim_cb != NULL) dim_cb(lit_brightness());
}

void display_sleep_set_hold(bool new_hold)
{
    hold = new_hold;
    if (!new_hold) lv_display_trigger_activity(NULL);
}

void display_sleep_wake(void)
{
    if (!asleep) return;
    asleep = false;
    /* Clear all bedtime bookkeeping: after light sleep the tick jumps, and
       stale settling/timers were re-fading the screen the instant it woke. */
    settling = false;
    pet_asleep_since_tick = 0;
    lv_anim_delete(blanket, fade_exec);
    lv_obj_add_flag(blanket, LV_OBJ_FLAG_HIDDEN);
    pet_freeze(false);
    if (state_cb != NULL) state_cb(false);
    lv_display_trigger_activity(NULL);
    if (dim_cb != NULL) {
        /* Ship the wake frame to the panel while it's still dark, THEN
           light it — brightness-first shows the write in progress (a
           guaranteed tear at every wake). */
        lv_refr_now(NULL);
        lv_timer_t *timer = lv_timer_create(wake_backlight_cb, 80, NULL);
        lv_timer_set_repeat_count(timer, 1);
    }
}
