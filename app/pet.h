#pragma once

#include <stdint.h>
#include "lvgl.h"

/* Creates the pet (placeholder critter until real sprites land) centered in parent. */
lv_obj_t *pet_create(lv_obj_t *parent);

/* Called when new steps arrive so the pet can react (hop, get excited). */
void pet_notice_steps(uint32_t delta);
