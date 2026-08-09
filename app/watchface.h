#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/*
 * Builds the full watchface (clock, pet, step count, goal ring) on parent —
 * the 410x502 panel: the screen on device, a panel container in the sim.
 */
void watchface_create(lv_obj_t *parent);

/* Updates the step count label and the daily-goal ring. */
void watchface_set_steps(uint32_t total);

/* Shows/hides the recording indicator while the record button is held. */
void watchface_set_recording(bool recording);

/* Shows a centered status banner (e.g. "WIFI SETUP"), or hides it with NULL. */
void watchface_set_banner(const char *text);

/* Battery indicator (top right). percent < 0 hides it. */
void watchface_set_battery(int percent, bool charging);
