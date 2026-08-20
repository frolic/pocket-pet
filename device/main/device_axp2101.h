#pragma once

#include <stdbool.h>

/* True while USB power is present (AXP2101 VBUS-good). */
bool axp2101_vbus_present(void);

/* Full PMIC power-off (COMMON_CONFIG bit 0): cuts every rail, including the
   panel's — the software equivalent of the 10s PWR hold, and the only thing
   that clears a latched CO5300. Wake afterward is a PWR press. */
void axp2101_power_off(void);
