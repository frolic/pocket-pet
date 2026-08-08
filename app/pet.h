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

/* Finger down/dragging on the field: he rotates to face the finger (parent coords). */
void pet_face_toward(int32_t x, int32_t y);
/* Finger lifted: he turns back to face the viewer and resumes idling. */
void pet_face_end(void);
