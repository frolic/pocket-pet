#pragma once

/* Starts the dark-time manual light-sleep loop (sleeps in 40ms quanta while
   DOZING on battery; PWR/BOOT wake). See device_sleep.c for the rationale
   and hazards. */
void device_sleep_init(void);
