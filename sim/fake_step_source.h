#pragma once

#include <stdint.h>

/* Starts the simulated walker that feeds step_source_total(). */
void fake_step_source_start(void);

/* Debug: adds a burst of steps immediately (milestone testing). */
void fake_step_source_add(uint32_t count);
