#pragma once

#include <stdint.h>
#include "lvgl.h"

/* Creates the pet centered in parent. */
lv_obj_t *pet_create(lv_obj_t *parent);

/* Called when new steps arrive so the pet can react (wander, get excited). */
void pet_notice_steps(uint32_t delta);

/* Record-button edges: he turns to face you and listens while held... */
void pet_listen_start(void);
/* ...and acknowledges with a nod on release. */
void pet_listen_end(void);

/* Tap on the field: he walks over to investigate the spot (parent coords). */
void pet_call_to(int32_t x, int32_t y);
