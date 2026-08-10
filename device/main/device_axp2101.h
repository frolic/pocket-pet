#pragma once

#include <stdbool.h>

/* True while USB power is present (AXP2101 VBUS-good). */
bool axp2101_vbus_present(void);
