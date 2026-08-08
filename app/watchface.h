#pragma once

#include <stdint.h>

/* Builds the full watchface (clock, pet, step count, goal ring) on the active screen. */
void watchface_create(void);

/* Updates the step count label and the daily-goal ring. */
void watchface_set_steps(uint32_t total);
