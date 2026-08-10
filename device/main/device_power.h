#pragma once

#include <stdbool.h>

/* Enables frequency scaling + light sleep plumbing (starts at full speed). */
void device_power_init(void);

/* Full clocks + no light sleep (true), or relaxed clocks + light sleep. */
void device_power_set_full(bool full);
