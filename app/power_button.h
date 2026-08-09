#pragma once

#include <stdbool.h>

/*
 * The power button: screen on/off toggle. Returns true once per short
 * press (consume-on-read). Sim: debug-rail PWR button. Device: AXP2101
 * PMIC short-press IRQ (long press stays hardware power-off).
 */
bool power_button_pressed(void);
