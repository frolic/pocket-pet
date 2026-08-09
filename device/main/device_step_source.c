#include "lvgl.h"
#include "step_source.h"

/*
 * Placeholder walker (~2 steps/second) until the QMI8658 hardware pedometer
 * is wired up — lets the whole game loop run on device meanwhile.
 */
uint32_t step_source_total(void)
{
    return lv_tick_get() / 500;
}
