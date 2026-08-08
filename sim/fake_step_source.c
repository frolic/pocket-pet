#include <stdlib.h>
#include "lvgl.h"
#include "fake_step_source.h"
#include "../app/step_source.h"

static uint32_t total_steps;
static int walking = 1;
static int phase_ticks_left = 10;

/* Walks in bursts with pauses, roughly human cadence at the 600ms tick. */
static void step_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (--phase_ticks_left <= 0) {
        walking = !walking;
        phase_ticks_left = walking ? 8 + rand() % 25 : 5 + rand() % 15;
    }
    if (walking) total_steps += 1 + rand() % 2;
}

void fake_step_source_start(void)
{
    lv_timer_create(step_tick, 600, NULL);
}

uint32_t step_source_total(void)
{
    return total_steps;
}
