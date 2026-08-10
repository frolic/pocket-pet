#pragma once

#include <stdbool.h>

/* Reads the first touch point directly from the FT3168 (LVGL bypassed).
   True while touched; fills panel coordinates. */
bool device_touch_raw_get(int *x, int *y);
