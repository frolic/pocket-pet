#pragma once

#include <stdbool.h>
#include <stdint.h>

/* True while USB power is present (AXP2101 VBUS-good). */
bool axp2101_vbus_present(void);

/* Full PMIC power-off (COMMON_CONFIG bit 0): cuts every rail, including the
   panel's — the software equivalent of the 10s PWR hold, and the only thing
   that clears a latched CO5300. Wake afterward is a PWR press. */
void axp2101_power_off(void);

/* Bench-only raw register write (the `pmicset` console command), for live
   rail experiments with eyes on the glass. The boot rail-set reasserts
   stock values on the next reset, so a bad poke never persists. */
bool axp2101_register_write(uint8_t reg, uint8_t value);
