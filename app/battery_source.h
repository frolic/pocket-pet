#pragma once

#include <stdbool.h>

/*
 * Battery state feed for the watchface. Sim: fake presets cycled from the
 * debug rail. Device: AXP2101 fuel gauge.
 */

/* Charge level 0-100, or -1 when the gauge is unreachable. */
int battery_source_percent(void);

bool battery_source_charging(void);
