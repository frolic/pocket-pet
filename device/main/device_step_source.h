#pragma once

#include <stdbool.h>

/* Device-side extras beyond the app-facing step_source.h: the dark-time
   light-sleep loop (device_sleep.c) paces accel sampling itself, one sample
   per ~40ms wake, while the normal 40ms sampling task stands down. */

/* True: the sampling task skips its own reads; the caller must invoke
   step_source_sample_now() at ~40ms cadence instead. */
void step_source_external_pacing(bool external);

/* One accel read + step-detector update + periodic NVS persist. Safe to call
   from any task; also serializes behind any in-flight I2C transaction, which
   the sleep loop relies on to quiesce the bus before sleeping. */
void step_source_sample_now(void);
