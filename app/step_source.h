#pragma once

#include <stdint.h>

/*
 * Where steps come from. The app only ever polls this total; it never talks
 * to hardware. Sim build: fake generator (sim/fake_step_source.c).
 * Device build: QMI8658 hardware pedometer over I2C.
 */
uint32_t step_source_total(void);

/* Diagnostic: 0 = pedometer configured OK, nonzero = init failure code
   (device-specific; sim always 0). */
int step_source_status(void);
