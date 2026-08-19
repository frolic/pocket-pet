#pragma once

#include <stdbool.h>

/*
 * The power button. Short press: screen on/off toggle. Long press (~1.5s,
 * the PMIC's long-press IRQ — distinct from the 8-10s hardware power-off):
 * brightness boost. Both consume-on-read. Sim: debug-rail PWR button
 * (no long press).
 */
bool power_button_pressed(void);
bool power_button_long_pressed(void);

/* Bench diagnostic (device only): live INTSTS2 watch, 8s. */
void power_button_watch(void);

/* Bench diagnostic (device only): dump PMIC rail enables/voltages. */
void power_rails_dump(void);
