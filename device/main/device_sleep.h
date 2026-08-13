#pragma once

/* Starts the dark-time manual light-sleep loop (sleeps in 40ms quanta while
   DOZING on battery; PWR/BOOT wake). See device_sleep.c for the rationale
   and hazards. */
void device_sleep_init(void);

/* Sleep telemetry (NVS-persisted; survives unplugged sessions): printed at
   boot and on demand — the instrument for battery-drain diagnosis. */
void device_sleep_stats_print(void);

void device_sleep_stats_reset(void);
