#include "lvgl.h"
#include "frolic_app.h"
#include "step_source.h"
#include "watchface.h"
#include "pet.h"

static uint32_t last_total;

static void poll_steps(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    uint32_t total = step_source_total();
    if (total == last_total) return;
    watchface_set_steps(total);
    pet_notice_steps(total - last_total);
    last_total = total;
}

void frolic_app_init(void)
{
    watchface_create();
    watchface_set_steps(step_source_total());
    lv_timer_create(poll_steps, 400, NULL);
}
