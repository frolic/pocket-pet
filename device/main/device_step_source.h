#pragma once

#include <stdbool.h>

/* Device-side extras beyond the app-facing step_source.h: the dark-time
   light-sleep loop (device_sleep.c) paces FIFO draining itself, one drain
   per ~1s wake, while the normal 1s draining task stands down. */

/* True: the draining task skips its own drains; the caller must invoke
   step_source_drain_now() at ~1s cadence instead. */
void step_source_external_pacing(bool external);

/* Drain the accel FIFO in one burst and run the step detector over the
   batch (per-sample timestamps), plus a periodic NVS persist. Safe to call
   from any task; also serializes behind any in-flight I2C transaction,
   which the sleep loop relies on to quiesce the bus before sleeping. */
void step_source_drain_now(void);

/* Walk recorder: dump / clear the PSRAM magnitude ring (console). */
void step_source_walklog_dump(void);
void step_source_walklog_clear(void);
