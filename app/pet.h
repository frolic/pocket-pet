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

typedef enum {
    PET_CELEBRATION_SHOCK,  /* electric burst — the big one */
    PET_CELEBRATION_HOP,    /* authored jump-for-joy */
    PET_CELEBRATION_BREATH, /* calm deep breath */
} pet_celebration_t;

/* Milestone celebration: turns to face the viewer and plays the animation. */
void pet_celebrate(pet_celebration_t kind);

/* Freezes all animation (static rest pose) — used while the radio works,
   since animation flushes and heavy wifi activity corrupt each other. */
void pet_set_paused(bool paused);

/* Suspends animation without changing state — used while the screen sleeps. */
void pet_freeze(bool frozen);

/* Begins the bedtime chain (turn, lie down, curl up) if he's free to. */
void pet_sleep_now(void);

/* True once he's in the curled sleeping loop. */
bool pet_is_sleeping(void);
