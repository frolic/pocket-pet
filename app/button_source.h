#pragma once

#include <stdbool.h>

/*
 * The physical record button. The app polls the held state and derives
 * press/release edges. Sim: debug-rail button (sim/debug_panel.c).
 * Device: the side BOOT button GPIO.
 */
bool button_source_held(void);
