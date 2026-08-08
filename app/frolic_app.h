#pragma once

#include "lvgl.h"

/*
 * App entry point, portable across sim and device: builds the watchface on
 * parent (the 410x502 panel) and starts the step + button polls.
 */
void frolic_app_init(lv_obj_t *parent);
