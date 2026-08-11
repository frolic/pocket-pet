#pragma once

#include <stdbool.h>

/* PCF85063 battery-backed RTC (0x51 on the BSP I2C bus). Restore feeds the
   system clock at boot so the wifi clock sync can be skipped; store writes
   the system clock back after every SNTP correction. */

/* Reads the RTC into the system clock. False when the RTC has lost power
   since it was last set (oscillator-stop flag) or holds nonsense. */
bool device_rtc_restore(void);

/* Writes the current system time (UTC) into the RTC and clears the
   oscillator-stop flag, making future boots instant. */
bool device_rtc_store(void);

/* True if this boot's restore succeeded (the clock is already honest). */
bool device_rtc_time_valid(void);
