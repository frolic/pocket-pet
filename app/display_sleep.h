#pragma once

#include <stdbool.h>
#include "lvgl.h"

/*
 * Screen sleep: after a period without input the display blanks (a top-layer
 * overlay swallows touches; a hardware hook cuts AMOLED brightness) and pet
 * animation freezes. Steps keep counting — the IMU runs independently. Waking
 * is explicit (the side button) via display_sleep_wake().
 */

void display_sleep_init(uint32_t timeout_ms);

/* Registers the platform brightness hook (on=true means visible). */
void display_sleep_set_hw_cb(void (*hw_cb)(bool on));

bool display_sleep_is_asleep(void);

/* Any user activity: postpones (or does nothing if already asleep). */
void display_sleep_poke(void);

/* Wakes the display (side button). */
void display_sleep_wake(void);
