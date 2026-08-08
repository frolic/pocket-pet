#include "lvgl.h"
#include "frolic_app.h"
#include "button_source.h"
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
    pet_notice_steps(total - last_total);
    last_total = total;
}

static void poll_button(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    bool held = button_source_held();
    if (held == button_was_held) return;
    button_was_held = held;
    watchface_set_recording(held);
    if (held) {
        pet_listen_start();
    } else {
        pet_listen_end();
    }
}

void frolic_app_init(lv_obj_t *parent)
{
    watchface_create(parent);
    watchface_set_steps(step_source_total());
    lv_timer_create(poll_steps, 400, NULL);
    lv_timer_create(poll_button, 50, NULL);
}
